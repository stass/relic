# Bulk fact construction and import

This document covers construction of base facts between input decoding and
the load commit. Private staging uses the storage representation that an
empty base relation can take over. Table imports decode bounded windows,
resolve distinct values and classify tuples before publishing an admitted
window's values and staged rows. Workers perform independent work; one
coordinator preserves input order, value identity, diagnostics and the
all-or-nothing base-fact commit.

Related: [architecture](architecture.md#facts), [facts](design-facts.md),
[tables](design-tables.md), [values](design-values.md),
[storage](design-storage.md), [memory](design-memory.md#the-memory-limit),
[parallel loading](design-parallel.md#loading-in-parallel),
[pending values](design-pending-values.md), [testing](design-testing.md),
[implementation plan](../docs/todo-benchmark-performance.md#4-build-and-load-base-relations-in-bulk).

## Model

A **load** owns private fact builders until its input and commit preparation
succeed. Each builder holds one predicate's distinct facts that its base
store does not already contain. A **relation payload** owns packed domain
columns, the striped canonical index, their capacities and tuple count.
Both a builder and an ordinary relation use this storage-owned payload
representation. A published relation also has its identity, statistics and
secondary-index metadata.

A **window** is a bounded amount of table input whose records can be
processed speculatively. A **piece** is a contiguous run of complete
records within a window. Pieces follow input order and are independent of
which worker executes them. A **prepared value** is a validated integer or
byte value with a private local token; it is not yet a context identifier.
An **admitted window** has a proof that every scalar field and record
operation in that window can pass the deterministic admission checks.

```text
program text / fact API ---- scalar value handling ----+
                                                     |
table stream -> bounded record framing                |
             -> private decode, dictionaries, tuples  |
             -> prove complete window admissible      |
                  | yes: reserve, publish values      |
                  |      and append private rows -----+
                  | no: replay scalar events ----------+
                                                     |
                                   private fact builders
                                                     |
complete input -> prepare commit -> discard -> reserve all destinations
                                                     |
                            adopt empty destinations / append populated ones
```

Publishing values and committing facts are different operations. Values
interned while reading a load remain interned if a later record fails;
base facts are published only once the whole load succeeds. The window
boundary preserves this distinction instead of making value publication
an all-or-nothing transaction for the entire input.

### Scope and reference path

Program text, CSV/TSV imports and `relic_fact_add()` share the private
builder and final commit. The bounded window pipeline applies to CSV and
TSV, whose schema is fixed before reading. Program text keeps the existing
serial parser: it can mix rules, queries and facts, has statement recovery,
and interns names and constants at its own semantic boundaries. A period
or newline is not a general program-text partition boundary.

`RELIC_F_NO_BATCH_IMPORT`, exposed as `-X no-batch-import`, selects the
scalar field-event importer at every worker count while retaining the same
builders and commit. This provides an independent reference for decoding,
interning and refusal order. Existing public function signatures and
configuration layout remain unchanged; the reference control uses an
available flag bit.

Execution policy chooses between the two paths; it adds no parser or
publication mechanism of its own. A window's analysis costs more than the
scalar importer's single pass, and repays it only shared among workers, so
the scalar importer is the production path for fewer effective workers
than `RELIC_IMPORT_BATCH_WORKERS` and for a table that ends within its
first window's horizon, which also spares a small import the pool's start
and the batch preparation. A table is batched when its input is
positively identified as a regular file, the context's pool runs at least
`RELIC_IMPORT_BATCH_WORKERS` workers, a measured count below which
batching does not repay its preparation, and reading its first window's
horizon neither reaches its end nor fails. The count is of workers running, not
threads configured: a forked child's pool runs one, and a pool whose
threads did not all start runs fewer. A pool that cannot be made leaves
the table to the scalar importer, as it leaves a round to the serial
evaluator. `RELIC_F_PARALLEL_ALWAYS`, `-X parallel`, batches every table
read from a regular file at any worker count, so that tests and
benchmarks exercise the pipeline whatever production scheduling selects.
Both paths stage the same facts and intern the same values under the same
identifiers, so the choice is visible only in time, memory and how far a
refused import has read.

**Only a regular file is batched.** A window is read before any of its
fields is analyzed, so on a pipe, a socket, a terminal or a stream of the
caller's own without a descriptor, a producer that sends a malformed
record and then waits would see its error reported only once a window,
or with framing ahead two windows and a transport chunk, had arrived, or
never. The importer identifies
its input by `fstat(2)` on the stream's descriptor and batches only when
that names a regular file; a stream without a descriptor, or one
`fstat(2)` cannot describe, is not one. A path is not evidence, since it
can name a FIFO; a stream the caller opened on a regular file is, standard
input redirected from one among them. Every other input takes the scalar
consumer before the first window is read, keeping its 64 KiB transport
and so reporting an error once the transport chunk holding it has
arrived, with the same builders and commit, and `RELIC_F_PARALLEL_ALWAYS`
does not override this. The scalar transport's own reads may block until
a chunk has arrived, so this keeps the scalar importer's responsiveness
rather than promising a report after a few malformed bytes. The manual
pages state the restriction wherever they describe batching.

## Invariants

* **A payload has one owner.** Ownership belongs to a builder or a published
  relation. Adoption consumes the builder's payload; cleanup cannot free it
  twice or leave it owned by staging after publication.
* **Only the final load commit adds base facts.** Successful windows add
  values and private staged rows, never a prefix of the public fact set.
* **Every stored tuple is indexed.** Private builders and published base
  relations have complete canonical indexes. Only published relations can
  acquire secondary indexes.
* **Identifiers retain global first appearance.** New integers and new
  byte-pool entries receive their ordinary increasing identifiers in input
  order. Worker number, completion order, partition and hash bucket do not
  contribute bits or ordering to an identifier.
* **An unproved window changes no semantic state.** Workers cannot intern,
  stage into the persistent builders, report a diagnostic or write a base
  store. The scalar replay begins at the same accepted input boundary.
* **A proved apply cannot fail.** Allocation, capacity admission and pool
  preparation precede value publication and builder append. Worker jobs in
  apply write only reserved, disjoint storage.
* **No mutable context overlaps worker reads.** Every phase that can grow
  context arrays follows the barrier ending all reads of those arrays.
* **Batching preserves persistent capacities.** The existing stepwise growth
  projectors determine value and relation capacities at every thread count.
* **Staging and committed storage have distinct accounting.** Input-sized
  private staging keeps the load's existing accounting exemption. Adopted
  capacity enters the storage account exactly once, at commit.

## Private fact builders

The storage layer owns payload allocation, membership, growth projection,
append into reserved space, release and ownership transfer. Fact staging
does not construct a fake registered relation or bypass accounting through
a flag on the ordinary insertion API. A builder type exposes private
construction; the base-commit receipt is the only way to publish its data.

`lib/facts.c` retains the existing per-predicate order and lookup interface.
Register a predicate on its first staged fact even when that fact is already
present in its base store. This keeps the first-growing-predicate diagnostic
in the original first-mention order. Each record's tuple is then checked
against the private builder and the frozen base store. An existing tuple
needs no retained row or "new" mark. A new tuple receives the next builder
identifier in first-input-occurrence order.

The load owns the context operation, so base stores cannot change between
these checks and commit. Builders keep the base relation's identity and
the context's hash seed; adoption never transfers data between contexts.
Arity is fixed by predicate identity. Arity-zero tuples use the shared
storage helpers and their empty-tuple representation.

### Storage that can be adopted

The builder's columns and canonical index are the final representation
of an initially empty base store. Its canonical stripes contain ordinary
tuple identifiers, and use the same unkeyed stripe routing, keyed table
probing and growth policy as published storage. There is no separate flat
staging index to translate at commit. Staging lookups mostly miss, so the
builder's stripes also keep the identifier table's per-slot tags, which
let a probe pass unrelated entries without reading their columns. Tags
filter candidates and choose no slot: adoption releases them as it moves
the payload, leaving the slots and capacities an ordinary insertion
sequence would have built, and the admission check measures the payload
without them.

A scalar append computes a tuple hash for its membership checks and
insertion. A batch carries hashes with its window until the builder append
finishes. Growing a builder's canonical index can rehash its existing
columns as an ordinary relation does. No hash or newness array survives
for every staged tuple across the whole load merely to support commit.

An empty destination adopts this complete payload without allocating a
second tuple array or rebuilding its canonical index. A populated
destination keeps its existing payload and appends the builder's new
rows. Its commit preparation can compute hashes and stripe work once over
those rows and retain that temporary work until apply. The first-load
memory benefit does not depend on an append-only scratch array that would
reintroduce per-row storage for empty destinations.

### Incremental admission

Scalar staging retains the existing checks after each new tuple:

```text
base tuples at load start + new tuples staged <= max_tuples

current value bytes + current pool bytes
    + columns floor for loaded predicates <= max_memory
```

Unset limits keep their usual meaning. The columns floor is the same
count-based lower bound used by ordinary staging, including the existing
rows of each predicate as it enters staging. It excludes canonical index
capacity and derived relations; commit checks actual destination storage
after discard. A failed staging check stops the parser or importer at that
fact, with earlier interned values retained and derived relations intact.

The builder's private capacity is not substituted for this floor. Doing
so would change which input record refuses a load and would charge staging
beside derived data the commit is about to discard.

## Preparing and committing the load

At end of input, prepare a commit manifest in predicate first-mention
order. Each growing destination has its projected final storage and one
action: consume the builder payload into an empty destination, or append
its rows to a populated destination. This choice depends on the base
store's tuple count, not on whether its header happens to exist.

Prepare append hashes, stripe work and manifest metadata before discard.
A failure here changes no base contents and keeps derived state, as a
failure collecting ordinary growth descriptions does. A duplicate-only
load has no action and performs no discard or reservation.

The final commit has three phases:

1. **Discard derived state.** Remove derived relations and base secondary
   indexes through the existing discard funnel. No base tuple is removed.
2. **Admit and reserve every destination.** Measure the whole post-discard
   account. An adoption projects replacement of any existing empty payload
   by the builder's actual capacity; an append uses ordinary relation growth.
   Check limits together. Release every superseded empty destination
   payload before any append reservation, then create missing base headers
   and reserve all append space before publishing any action. A refused
   reserve can leave empty headers, released empty capacity or additional
   append capacity, accounted, but no new facts.
3. **Apply every action without failure.** Adoption moves the payload into
   its destination and empties the builder owner. Append copies new rows
   to their reserved positions and places their preassigned identifiers in
   canonical stripes. Reconcile storage and tuple totals from the resulting
   owners. Then release append work and the empty staging owners.

A pre-existing empty base header remains the same object. Releasing its
unused payload during preparation changes no tuple and makes any replacement
credit real before another reservation spends it. Otherwise an aggregate
projection could fit only because it subtracts empty capacity that the
ordinary append reserver still sees charged. No rollback of that unused
capacity is required after allocation failure. No new destination columns
are allocated for adoption. Relation metadata is initialized for the adopted
contents, with no secondary indexes or cached distinct-column counts from
another span.

For an append, builder tuple `i` becomes base tuple `old_count + i`.
The append shares the canonical stripe work of a round's merge
([storage](design-storage.md)) -- its stripe list, its prepared
growths, the part that fills a stripe and the cleanup of growth no part
completed -- but has a constructor of its own: it borrows the builder's
immutable columns rather than copying them, is built before discard
without regard to the destination's secondary indexes, and its
workspace is load staging, owned and released by the commit's receipt
and charged to nothing.

* **Preparation**, before discard, computes each builder tuple's wide
  hash, the stripe counts and the list of tuples by stripe.
* **Reservation** grows the destination as an ordinary relation
  reservation does -- the columns grown once to hold the new tuples, each
  canonical stripe reserved for its count -- except that a stripe's
  growth may be prepared and completed by its part ([final size and
  peak](design-values.md#the-identifier-table)).
* **Apply** copies the builder's columns after the destination's last
  tuple, then runs one part per canonical stripe, which completes any
  deferred growth and places the stripe's tuples in builder order.
  Copying publishes no count: after the barrier the coordinator adds the
  tuples to the relation's count, the context's total and the base total,
  once each, and reconciles the storage total.

Admission asks the final sizes that ordinary relation reservation
reaches, the same projection reservation then realizes, and each stripe
reaches its capacity whether its growth was deferred or not.

The parts run on the context's existing pool, adopted into the current
process, when it has more than one worker and the commit's appends hold
at least `RELIC_COMMIT_PARALLEL_TUPLES` tuples, a measured count below
which waking the pool costs more than it saves, or whatever their count
under `RELIC_F_PARALLEL_ALWAYS`, so that small loads exercise the shared
path; otherwise they run on the calling thread, and no growth defers.
The parts are the same either way. A commit never makes a pool. Job
storage is prepared before the no-failure phase; apply runs to completion
without a failure or callback after publishing only some destinations.

The receipt owns all resources needed to finish or clean up preparation.
It holds the actual builder/destination owners, not an index into an array
that a later allocation can relocate. Once apply begins there is no undo
of base tuples. A failed preparation leaves builders owned by staging,
so the load's ordinary cleanup releases them.

A program-text load still splices its already prepared statements after
the fact commit. A path import successfully closes its input before commit,
as `import_named_file()` does. An import of a caller-owned `FILE *` never
closes that stream.

## Bounded table input

The importer copies its schema, predicate name and source name before
calling input code. Read callbacks can modify the caller's original buffers;
workers use only these owned copies. Predicate interning and header handling
stay on the calling thread before batch data processing. Headers are checked
for structure and width but neither typed nor interned.

The calling thread reads input into a bounded window and records valid
record boundaries. Piece sizes and window limits are fixed internal byte
and record targets, independent of the configured worker count. They are
tuning parameters, not language limits. A window cannot extend across an
arbitrarily large record merely to fill the worker pool.

A window is defined from its own first byte. Its horizon is
`RELIC_IMPORT_WINDOW_BYTES` from that byte, and its record cap is
`RELIC_IMPORT_WINDOW_RECORDS`: the window is the whole records the
reader completes in the bytes before the horizon, or before the end of
input when that comes first, up to the cap, a header among them, and the
rest of the input after its last record begins the next window. How the
stream was read -- where its transport chunks fell, how far past the
horizon it has been read -- decides nothing. A transport buffer ending
inside a normal record is not a reason to abandon batching. A record
that alone exceeds the horizon takes scalar continuation instead of an
unbounded window allocation.

**The record cap bounds a window's scratch.** A window's input is
bounded by its horizon, but its analysis scratch grows with its records:
each one's tuple words, wide hash, stripe, stripe-list entry, rank and
screen slots, about 32 bytes for a record of one kept column. Without a
cap a window of records of one byte and a line feed, a million of them,
would take some 32 MiB of scratch for its 2 MiB of input, none of it in
the memory account. The cap is the horizon's bytes over eight, 256 Ki
records for the 2 MiB horizon. A window is cut by it only when its
records average fewer than eight bytes, which no measured benchmark
table's do over the whole table (they average nine to twenty-three), and
a window it cuts takes about the 8 MiB of scratch a window of eight-byte
records already takes. A lower cap would cut the windows of the measured tables,
and with them the pieces a window shares among workers; whether that
would repay is a measurement, not a bound. The tuple words of wider
records are bounded by the horizon in any case: each column costs at
least a delimiter byte of input.

**The end of input at the horizon.** The bytes before the horizon are
fed to the reader as the table's last only when the stream has ended
successfully and the end lies strictly before the horizon. Reaching the
horizon is not the end of input, and neither is an end found exactly at
it: a final record no line feed ends, lying across or exactly up to the
horizon, belongs to the next window, which begins with it and holds the
end strictly before its own horizon. So whether the reader has read
past the horizon, or found the stream's end there, before it frames a
window cannot change which records the window holds. Reaching the cap
is not the end of input either: a window the cap ends is the table's
last only when the bytes before its horizon were fed as the table's last
and its last record ends where they do, which is where the reader, read
on, would meet the end.

One framing implementation serves every window: a framer begun at a
window's first byte, fed at most `RELIC_IMPORT_READ_CHUNK` of buffered
bytes a step, records each whole record into the window's manifest of
pieces until it reaches the horizon, the record cap or the end of input,
or finds a mistake or runs out of memory, which leaves the window
unframed and takes scalar replay from its boundary. The cap is checked
as each record is framed, before another byte is read, so a window the
cap ends holds the same records however its framing is stepped. Framing
a window in place and framing it ahead are the same steps, run on
different threads.

### Framing ahead

Framing is one serial pass of the reader over a window, and its next
boundary is known only once the current window's framing ends. The
calling thread frames the next window while the current one is analyzed,
in steps run between and instead of its own analysis parts. The calling
thread is worker 0 of every job, and the table reader is incremental, so
framing needs no thread of its own and no job that outlives a barrier.

* **The analysis jobs carry the framer as their filler.** Worker 0 runs
  framing steps while some other worker is still at work, then claims
  any parts left and counts itself in
  ([a job's filler](design-parallel.md#the-worker-pool)). A job's
  completion can therefore wait for one framing step in progress, never
  for the framing that remains. Only the frozen-read analysis jobs --
  decoding, finding, resolving and classifying -- carry it; the stripe
  fills and every other job that publishes do not, and a job that runs
  on the calling thread alone runs no step.
* **The framer outlives a job, not a window.** The framing state -- the
  reader's state, the next window's manifest and how far it has been fed
  -- belongs to the importer, so the framing one job's filler leaves
  undone continues in the next job's, and what the last job leaves is
  finished on the calling thread once the current window is staged,
  before the next one is analyzed.
* **A step frames buffered bytes and reads nothing.** A step feeds the
  framer at most `RELIC_IMPORT_READ_CHUNK` of bytes already in the buffer
  and records the records it completes as the next window's pieces. It
  never reads the stream, so it neither blocks nor runs a stream's
  callbacks while parts run.
* **The caller reads between jobs.** Before a window's analysis
  dispatches, the caller reads the stream in transport chunks until the
  buffer holds the next window's horizon, that is the current window and
  a horizon of bytes after it, or the stream ends or fails. Read-ahead
  past the accepted boundary is therefore bounded by two windows plus at
  most one transport chunk, the read that crosses the next horizon. Only
  a regular file is batched, so these reads wait on storage, never on a
  producer. Every read happens on the calling thread outside a job,
  records its outcome as read-ahead always does, and reports nothing.
* **One framer, three positions.** The input adapter distinguishes the
  accepted boundary, where replay begins; the framing cursor, which owns
  the framer's parser state and the next window's start; and the
  buffered end. The framer is fed a span only after it has consumed the
  previous one, and only the end-of-input rule above marks a feed as the
  last; a recorded read failure never does. The framer lives for one
  window's framing, and a partial record at its end is framed again, from
  its first byte, by the next window's framer.
* **Bytes move only between jobs.** Pieces record offsets from their
  window's start. The stream is read, and a consumed window's bytes
  dropped from the front of the buffer, only on the calling thread
  between jobs. The framer is begun only once the bytes it frames are
  read, and fixes then how many it holds and whether the stream ended
  after them; it holds none of them between steps and finds its base
  anew at each, and a window's base is set after the read-ahead. So
  neither a read that grows the buffer nor a consumption that shifts it
  is ever seen through a stale pointer.
* **Two framing manifests, one analysis workspace.** The current
  window's manifest -- its pieces, record counts and end -- is analyzed
  while the next one's is framed; analysis never overlaps analysis, so
  the decoded bytes, dictionaries and tuple arrays exist once. A step
  that cannot grow the next manifest marks it unframed, which takes scalar
  replay from its boundary as any unframed window does.
* **The next window is used only after the current one is staged.** Its
  framing began at the current window's end, which is the accepted
  boundary only once the current window is staged. A declined current
  window discards the next window's manifest, and scalar replay reads
  every buffered byte from the accepted boundary, then the recorded read
  outcomes, then the stream.

Framing ahead changes no boundary, piece, value, diagnostic or replay
position: the reader decides every boundary from the same bytes, in the
same order, whatever the steps' schedule. What it moves is time, and how
far the file has been read when a window's values are published, which
the batched and scalar paths already differ in. The serial phases of a
window -- gathering, numbering, admission, reservation and value
publication -- remain on the calling thread and frame nothing; framing
still bounds an import when it takes longer than the rest of a window's
work. Worker 0 gives up its share of the analysis parts while it frames,
so the steps are not taken only from idle time: framing ahead is kept
only where it beats framing each window before its analysis at the
batching worker counts, measured beside the frame-only reader's own
speed.

### One parser state machine

Framing and decoding run the same state machine, the pure table reader's
transitions. Framing observes boundaries and byte locations
without collecting decoded field payloads. Decoding emits the existing
field, record, end and mistake events. A sink or output policy changes
what is retained, not which transitions occur.

CSV quotes, doubled quotes, embedded newlines, CRLF and end-of-input states
therefore decide boundaries in exactly one place. TSV escapes and incomplete
escapes use those same transition rules for the TSV format. Counting quotes
or searching for newline in an independent splitter is not a parser.
Both passes have linear byte cost; measuring that repeated state-machine
work is part of the serial import acceptance test.

Each piece begins at a certified record boundary with the corresponding
source location. Diagnostic locations retain byte-based line and column
semantics. A diagnostic's displayed location is not an event-order key:
for example, an unterminated quoted field is discovered at end of input
but names its opening quote.

If a record exceeds the window bound, or framing cannot establish a clean
window, the importer switches to scalar processing from the last accepted
boundary. It consumes the already buffered prefix and continues streaming
through the same reader. No seek or reread of the external stream is needed,
and the pipeline does not retain an oversized record's entire raw text.
The scalar field buffer keeps the existing longest-field behavior.

### Read errors and read-ahead

Use the existing `RELIC_IMPORT_READ_CHUNK` transport unit and retain its
read outcomes while bytes are speculative. One input adapter reads the
stream for both batch and scalar processing: a short read with `ferror()`
drops the bytes that failed read returned and records the failure, which
is reported once every byte read before it has been consumed, rather than
treating the partial bytes as a successfully read prefix.

An I/O failure observed during read-ahead is recorded, not immediately
reported ahead of earlier buffered events. Scalar replay processes earlier
successful reads first, then observes the recorded failed read if it gets
that far. A read-ahead failure cannot replace an earlier field or staging
failure. A short successful read retains ordinary EOF semantics.

Read-ahead is bounded by two windows plus at most one transport chunk,
and only the caller reads or closes a stream. Only a regular file is
batched, so only a regular file is read ahead beyond the scalar transport
chunk. A stream the caller opens on a regular file, standard input
redirected from one among them, is batched like a path's; every input not positively identified as a regular file
takes the scalar consumer from the start and uses the same buffered input
adapter. Diagnostics run on the caller after all jobs that could refer to
their buffers have joined.

## Decoding and value dictionaries

Workers decode pieces independently into private data:

* ordered records of local value tokens for kept fields;
* a local dictionary of validated integers and byte values, with their
  hashes and first field-event positions;
* owned decoded bytes for distinct local payloads;
* a completion status saying whether the piece is entirely valid.

Skipped columns still undergo structural parsing but contribute no value
descriptor. Integers use the existing lexical conversion, including its
range checks. Symbols use the existing symbol grammar. Strings preserve
their full decoded bytes and lengths, including empty strings and embedded
NULs. Internal validated descriptors avoid checking an imported symbol's
grammar again during interning; public value APIs retain their validation.
An empty payload uses an addressable empty span, not pointer arithmetic
from a null byte buffer.

A worker records a refusal or parse error privately. It never calls a
diagnostic callback, interns a value, mutates a predicate or stages a row
into the load's persistent builders. Any such status makes the window
unproved; replay determines the first observable failure.

### Global equality, ordered identifiers

Partition local dictionary descriptors for private reconciliation by their
keyed hashes. A part owns its reconciliation table and visits piece
descriptors in input order, using exact integer or byte equality. It probes
existing context tables read-only, and finds one earliest representative
for each new semantic key across pieces. Colliding hashes still compare
complete payloads.

The byte-pool dictionary is shared by symbol and string descriptors: equal
bytes have one pool entry, with separate requested symbol/string use bits.
Integers form the ordinary separate dictionary. Reuse of an existing pool
entry can still require growth of the use-bit array; the highest affected
pool identifier, not just the number of new strings, controls that growth.

The coordinator walks the pieces' first-occurrence lists in input order
and numbers only winning new representatives. New byte entries receive
consecutive pool identifiers after the frozen pool count; new integers
receive consecutive integer identifiers after the frozen integer count.
Check each count-plus-rank against the identifier range before forming
the domain word; an unrepresentable promise declines to scalar replay.
Existing entries keep their identifiers. Combining the appropriate type
bits with the pool identifier gives distinct symbol and string values.
No sort by hash, worker or value controls numbering.

Local-token maps then give every occurrence its promised domain word.
For a byte token, the field's declared type supplies the type bits of the
resolved pool identifier; a byte representative shared by symbol and
string fields does not collapse their domain values. These maps are
scoped to the window. The value layer remains the authority
for equality, type-use bits, growth projection, reservation and publication.
Extend its batch facilities used by `relic_pending_measure()` to cover
validated symbol values as well as integers and strings; do not create
another persistent interner in the importer. The
[pending-value compaction](design-pending-values.md) path keeps its ordinary
integer/string inputs and shares those value-layer rules.

Successful numbering is the serial numbering: deleting a later equal local
occurrence cannot delete a semantic key's earliest input occurrence, and
cross-piece reconciliation keeps the earliest remaining representative.
The order of the new byte keys and of the new integers is therefore the
same as scalar field consumption.

## Classifying records

Rewrite each piece's record tokens to promised domain words in its private
buffer. Compute each tuple's normal hash once for this window. Tuple hashes
are over domain words, so they do not need to decode an unpublished value.
No such word reaches a published relation before its value is assigned.

Partition records by canonical stripe. Each part visits its records in
input order, probes the frozen builder and base store, and screens repeats
within this window. It marks only the first record new to all three sets.
An ordered, checked prefix count of those marks gives the append position
of each new builder tuple. Check its representation before narrowing it
to a tuple identifier. Index partitioning does not assign tuple identifiers.

Value discovery precedes record deduplication. The import's field handling
and type-use effects cannot be skipped merely because a completed record
turns out to be a duplicate. The classification result is private and
read-only until the window is admitted.

The global value tables, base stores and earlier builder contents stay
unchanged throughout dictionary lookup and tuple classification. Their
arrays cannot grow while these jobs run. A barrier ends these reads before
the coordinator begins reservation.

## Proving a whole window admissible

Batch publication is permitted only when the entire window is valid and
its monotone projections prove that every scalar prefix fits. Use the
existing pure growth functions, not another estimate of their allocation
policy. The proof includes:

* all promised integer, pool and tuple identifiers are representable;
* the whole context account with the final value/pool capacities fits the
  value interning checks while the existing derived state is still present;
* base tuples plus all new staged tuples fit the tuple limit;
* final projected value/pool bytes plus the staging columns floor fit the
  staging memory check.

The proof takes a const snapshot and returns admissible or declined. It
does not call mutating admission/reporting helpers or set the context's
error or limit-refusal state. A declined projection must leave scalar
replay's diagnostic state as untouched as its values and rows.

### One check per layer

The proof restates no scalar check. Each layer that can refuse a field
or a record exposes one pure check, which its scalar operation calls
before it changes anything and which the proof calls with the window's
totals:

| Layer | Pure check | Scalar caller | Proof's argument |
| --- | --- | --- | --- |
| Field | the verdict on a field's bytes under its column's type: symbol grammar, integer syntax and range, the pool's length ceiling | the field consumer, which turns a refusal into its diagnostic | each field, on its decoding worker |
| Value tables | room in the integer table and the pool for so many new entries | interning one value | the window's new integers and new byte strings |
| Staging | the tuple limit and the memory floor for so many new tuples of a predicate, given the value bytes, the floor including the predicate's stored rows when its set has not begun | staging one new tuple, with the account's current value bytes | the window's new tuples, with the projected value bytes |
| Builder | the builder's growth for so many new tuples: tuple identifiers, column words, column bytes and each stripe | adding one tuple | the window's new tuples by stripe |

The builder's growth is computed once, by one projection that checks
every count, product and capacity a reservation forms, including the
byte size of the stepwise column capacity whether or not a memory limit
is set. It returns either the validated capacities or the reason growth
is impossible; impossible growth is never represented as unchanged
bytes. Admission asks whether the projection exists, the scalar path
turns its reason into its diagnostic, and reservation realizes its
capacities and never computes one the projection did not. Zero growth
projects the builder as it is, and arity zero keeps the array's minimum
allocation. The field verdict keeps the scalar consumer's precedence
between refusals, a malformed symbol before an overlong one. A check
added to a layer is therefore added to both paths at once, and a window
the proof admits cannot reach a refusal its scalar reading would have
met earlier.

The string arena is projected in global first-byte-occurrence order, one
copy and terminator at a time, as ordinary interning allocates it. Arrays
and tables use stepwise capacity, including use bits for old pool entries.
All count sums, products, offsets and projections are checked before use.

These conditions suffice because the checked counts and capacities never
decrease as scalar events are consumed. Each intermediate pool growth,
type-use growth and new-record floor is no larger than its corresponding
final projection. An admitted window cannot encounter a deterministic
value or staging refusal in its scalar execution. The proof can decline a
window that scalar processing would accept; replay resolves that case.

The proof does not replace final base-commit admission. In particular,
canonical index capacity can cause a refusal at commit even after all
staging floors pass. Such a refusal remains at commit, after all input has
been handled, with the existing diagnostic and retained-value behavior.

### Scalar replay is the refusal path

Before proof succeeds, the current window has added no context values,
type-use bits or builder rows. If it is malformed, cannot be certified, or
private speculative work cannot be allocated, join its workers, release
its private analysis and replay its saved input through the ordinary
field-event consumer. Continue the remainder of that import on the scalar
path. The saved input provider delivers buffered bytes and recorded read
outcomes before reading more from the stream.

Speculative allocation helpers return failure without reporting it. A
failure already reported by operation setup, such as a refused pool setup,
is an ordinary operation failure and is not converted into a silent replay.

Replay uses the same value interning and per-record staging functions as
`no-batch-import`. It therefore establishes the exact semantic prefix on
failure, without ranking error classes or duplicating the scalar interner's
internal failure sequence. For example, a symbol can enter the byte pool
and then fail while growing its type-use record; that pool entry survives
just as it does in scalar import. A tuple-limit refusal retains the values
of the refused record but interns no later record.

Previously accepted windows remain staged and their values remain interned.
The replay starts after them, so it cannot renumber or double-apply them.
If replay fails, the load discards all private rows and adds no base facts.
A syntax/type/limit failure is reported once by the scalar consumer, with
its ordinary location and bounded description of the offending field.

## Reserving and applying an admitted window

After proof, prepare all space for the value additions and builder rows
before publishing either. The value reservation and builder reservation
produce an owned receipt. Once reservation starts, an allocator failure
uses the ordinary failure/cleanup contract; it is not a fresh speculative
decline to replay against context capacities that have already changed.
Retained capacities remain accounted, and uncommitted pool copies are
rewound through their reservation owner.

The coordinator's value reservation copies each new string's bytes and
terminator from the immutable piece bytes into a destination allocated in
identifier order through the existing arena policy, as ordinary interning
does, and the receipt owns those copies; publication initializes the
entries from them and allocates and copies nothing. The copies are not a
pool job: beside placing the entries in the global lookup tables they
cost too little to measure, and making them on the coordinator keeps
every write to the pool's entries and arena there. Empty strings have a valid destination; zero-length payload
copies are skipped. Review every `bcopy(src, dst, n)` for source-first
order.

The coordinator appends dictionary entries in the promised order and
records symbol/string uses. New integers are placed in the integer
table's index there too. New pool entries are placed in the pool's
striped index a stripe to a part on the pool, each part placing its
stripe's entries in identifier order
([the symbol pool](design-values.md#the-symbol-pool)). The window
carries each new byte value's wide hash from decoding, so no part hashes
bytes again. Identifier layout is unchanged: a stripe holds identifiers,
and which stripe holds one depends on its bytes alone. Numbering and
entry append stay a measurable serial phase.

With values live, append the admitted new rows into the builder's reserved
positions. Copying columns precedes canonical-index placement, and each
stripe has one writer. Append order and final capacities are those of
scalar staging. Advance the builder's count only after its apply barrier.
The receipt is then consumed, and all piece buffers, maps and temporary
screens can be released.

After the receipt is prepared, apply has no allocation or diagnostic
callback that can publish only part of its admitted work. Loading and
importing retain the [API's non-interruptible behavior](design-api.md#interruption);
their pool jobs do not use the evaluator's interruptible derivation loop.
The base-fact commit still waits for successful EOF and, for an owned path,
successful close.

## Ownership and memory accounting

| Owner | Storage and lifetime |
| --- | --- |
| Import operation | Copied schema/name metadata, stream adapter and replay position until return |
| Current window | Raw input, decoded distinct bytes, local dictionaries, token maps and record screens until apply or replay begins |
| Value reservation | Reserved pool copies and destination ranges until publication or rollback; no borrowed entry-array pointers survive growth |
| Fact staging | Per-predicate builder payloads and first-mention metadata until load commit or failure |
| Commit receipt | Destination reservations and append work until all actions apply or preparation fails |
| Published context | Adopted/appended base payloads, value tables and pool bytes through ordinary context lifetime |

Workers can borrow immutable pool byte pointers, whose addresses do not
move, but cannot retain pointers into growable entry arrays. Fresh payloads
remain owned by their window until the pool has copied them. A replay keeps
raw bytes and transport outcomes, not any speculative promised identifiers.

Private builders and window buffers are load staging, proportional to
input, and retain the existing exemption from `max_memory`. Windowing bounds
speculative memory independently of file length, the horizon bounding a
window's bytes and the record cap its per-record scratch; accumulated distinct new
facts still require staging until the all-or-nothing commit. The configured
memory limit is not an RSS or input-byte limit.

Adoption charges the destination's actual payload capacity through normal
store accounting. It does not charge both a builder and its moved payload,
nor leave adopted arrays uncharged. Replacing empty retained destination
capacity subtracts that old capacity. Appends charge actual reserved base
growth; failed reservations reconcile whatever capacity remains.

For a first load into empty destinations, the data-sized tuple storage at
commit is one builder payload per predicate, plus bounded window storage
and value storage. There is no second full tuple array or second final
canonical index. Appending to populated stores can hold both their enlarged
payloads and new-row staging; the design makes no zero-copy claim for that
case. Allocator retention and transient `realloc()` overlap remain separate
from accounted persistent capacity.

## Failure paths and limits

The existing context pool runs piece decoding, dictionary reconciliation,
token resolution, tuple classification and stripe append work as separate
jobs. Their barriers separate frozen reads from mutation. The calling
thread owns stream operations, all context reservations, numbering,
diagnostics, totals and final publication. Disabled-thread builds have one
worker, and so import through the scalar path unless
`RELIC_F_PARALLEL_ALWAYS` asks for batches, whose jobs they then execute
on the caller.

Worker-facing interfaces expose the pure table decoder, const value/store
lookups and private output owners. They do not expose a mutable context or
its accounting and publication APIs. Move the pure reader declarations out
of the broad internal interface as needed. The analysis jobs only read,
and are compiled against the existing frozen-store boundary. Apply jobs
write, so they cannot be frozen readers: each receives a descriptor of
only its stripe destinations, the reserved column rows and the one
canonical stripe table it fills, and no authority to grow a relation, add
to its tuple count or publish it. The coordinator retains that authority
through its receipt.

Pool setup and all job metadata are fallible preparation, never part of a
no-failure apply. A small import takes the scalar path rather than pay
for dispatch; that choice changes no framing, numbering, admission or
ownership rule. A worker-count change does not change the input pieces.

| Failure boundary | Observable result |
| --- | --- |
| Invalid import description or missing input | Existing argument/open diagnostic; no table data handled |
| Unproved window or private speculation refusal | Scalar replay from its saved boundary determines the input or limit failure, if any |
| Allocation failure during admitted reservation | One ordinary memory diagnostic; no base facts added; owned reservations cleaned up and retained capacity accounted |
| Later scalar record failure | Earlier accepted values remain; all staged rows are released; derived state remains until a commit begins |
| Owned input close failure | No base commit; values already interned remain |
| Commit admission/reservation failure | No new base tuples or statements; derived state has been discarded; retained destination capacity is accounted |
| Successful commit | Every new fact appears in its original per-predicate order and currency is invalidated as usual |

No callback or active worker can observe partially applied base actions.
Successful publication requires no rollback of tuple identifiers, index
contents or context value numbering. Query reuse after a failed load or
import follows the existing stale/current relation rules.

Untrusted bytes are validated through the same grammar and type helpers.
Private tables use the context's keyed hashes and exact equality. Offsets
and lengths are checked before pointer formation, and a worker never
decodes beyond its owned span. Diagnostic text uses the existing escaping
and length bounds; speculative fields never reach a callback.

## Determinism and representation choice

For the same pre-load context state and input bytes, global first-occurrence
numbering preserves successful scalar identifiers for integers and byte-pool
entries. Type bits preserve distinct symbols and
strings sharing bytes. New tuples are numbered by first record occurrence
within each predicate. Hashes distribute work, while numeric identities
and publication follow source order. Repeated runs, hash seeds and worker
counts give the same values, tuples and row order.

The stepwise projectors preserve persistent capacities across batching and
thread counts. A deterministic refusal takes scalar replay before the
current window changes contents, preserving the same failure and retained
semantic prefix. Allocator failure schedules need not coincide; they must
leave consistent contents, live pointers and exact accounting of retained
capacity. Private workspace and process RSS can differ by execution mode.

Loading still contributes no evaluation derivations, insertions, iterations
or parallel rounds. The existing base totals and value-use counts describe
the successful result. Timing and workspace instrumentation for this work
belongs in tests and benchmark tooling, not new public statistics solely
to demonstrate that jobs ran.

### Comparing identifier designs

| Choice | Benefit | Cost or constraint |
| --- | --- | --- |
| Fixed stripe bits and per-stripe numbering | Independent persistent interning per stripe | Changes identifiers and decoding, partitions the identifier space, and replaces global first appearance with per-stripe order |
| Private partitioned dictionaries and ordered publication | Preserves identifier encoding, existing lookup tables and all scalar identities | Number assignment and global table publication remain serial and must be measured |
| Partitioned persistent lookup indexes with global numbering | Allows parallel placement after global IDs are assigned, without stripe bits in those IDs | Changes permanent table layout, growth accounting and every interning path; needs evidence that publication is the remaining bottleneck |

The target uses private dictionaries and ordered publication because
identifiers are observable and independent work does not require changing
their encoding. It also partitions the pool's lookup index, the third
row, by the unkeyed stripe of each byte string's wide hash: sampled at
eight threads on 52 MB of distinct symbols, placing new entries in the
pool's single index was a fifth of the calling thread, the largest
publication step. Striping keeps global first-appearance numbering and
moves no identifier; it changes the pool's table layout, its growth
accounting and the hash every pool lookup is given, under the rules the
other striped tables already follow. The comparison is architectural; throughput is established
by phase measurements. Parallel import's acceptance records the cost of
ordered numbering, entry placement and reservation separately from worker
work on all-distinct as well as repetitive inputs, and a parallel loading
gain is claimed only where those serial phases do not dominate.
Changing identifier encoding requires a separate cross-subsystem
decision, not an implementation shortcut.
Persistent lookup partitioning can retain global identifiers; it is a
storage-layout alternative, distinct from partitioning the identifier space.

## Internal boundaries

| Component | Responsibility |
| --- | --- |
| `lib/relation.c`, `lib/relation.h` | Own the reusable payload representation, private builder operations, exact growth, adoption and prepared base appends |
| `lib/facts.c` | Keep predicate/input order, perform staging admission, prepare the load manifest and run its single commit |
| `lib/import.c` | Share parser transitions, own buffering/replay, produce validated records, coordinate windows and preserve input diagnostics |
| `lib/table.h` | Declare the pure table reader and the lexical field rules, which need no context, for the importer and the workers alike |
| `lib/window.c`, `lib/window.h` | Run a window's decode, find, resolve and classify parts, compiled against `lib/frozen.h` alone, each part writing only its own window slices |
| `lib/value.c`, `lib/symbol.c` | Reconcile values under canonical equality, assign ordinary identifiers, project/reserve storage, own byte-copy receipts and publish values/use bits |
| `lib/pool.c` | Execute disjoint jobs, run a job's filler steps on worker 0 and establish completion barriers, without loader-specific scheduling state |
| Source parser and fact API | Keep their scalar semantic boundaries while staging through the shared builder |

Immutable piece and reservation descriptions make data ownership visible
at job boundaries. The importer does not duplicate growth formulas or
directly install pointers into context relations or symbol tables.

## Validation and acceptance

Write the ownership regression before changing staging. Instrument the
storage allocation/transfer boundary for a large first load and require
that its destination takes the builder's columns and canonical storage
without a second full allocation. A test that merely compares answers
cannot detect the memory duplication. Cover an absent base header and an
existing empty one, and verify that ordinary cleanup leaves the adopted
storage readable and frees it exactly once at context destruction.

Use the scalar import switch as the reference for batch processing, and
retain existing fact/import failure tests. The bounded-import tests in
`tests/unit/test_import.c` already verify that the refused row's values
are interned and a later row's values are not; extend them across window
and piece boundaries. Keep value validation and parser unit tests as
independent checks rather than comparing two uses of the same new helper.

| Concern | Required evidence |
| --- | --- |
| Ownership transfer | Empty targets adopt; populated targets append; failure before apply transfers nothing; replacing a large empty payload releases its capacity before another destination reserves; repeated cleanup cannot free an adopted payload |
| Fact atomicity | Mixed predicates and a refusal in the last destination add no facts anywhere; program statements remain unspliced on failure |
| Duplicates and currency | Duplicates within pieces, across windows and against base stores preserve first occurrence; duplicate-only loads keep materialized relations current |
| Dictionary identity | Mixed symbols/strings sharing bytes, existing constructor-only bytes, high use-bit indexes, repeated integers and all-distinct values match scalar IDs and use counts |
| Framing | Every possible byte split around quotes, doubled quotes, embedded newlines, CRLF, TSV escapes and EOF agrees with the pure reader; skipped fields and headers still validate structure |
| Oversized records | The buffered prefix replays once into scalar continuation on nonseekable input, without retaining an unbounded complete raw record |
| Failure order | Type errors before later syntax errors, earlier staging limits before malformed tails, and read-ahead I/O failures produce the scalar diagnostic and interned prefix |
| Partial value effects | A pool append followed by refused use-bit growth, and a refused tuple after successful field interning, match the scalar state exactly |
| Limits | Whole-window proofs use final monotone capacity and the original staging floor; exact commit can still refuse; arity zero, near identifier limits and checked size arithmetic are covered without huge allocations |
| Transport and callbacks | Failed short reads discard their returned bytes, successful short reads end input, close failures precede commit, caller-mutated descriptions cannot affect copied plans, and interrupts preserve the existing non-interruptible import contract |
| Allocation failure | Nth-allocation sweeps cover private decline, value/builder reservation, pool copies, manifest preparation and late destination reserve; the context can load/query again afterward |
| Concurrency | 1, 2, 4, 8 and 16 workers and disabled-thread builds agree on IDs, capacities, diagnostics and output; instrumentation proves both bulk apply and replay paths actually execute |
| Striping | Symbol-pool and append capacities agree across hash seeds and thread counts, with deferred and immediate stripe growth alike; input crowded into one stripe keeps its answers, capacities and bounded probes |
| Framing ahead | Windows framed ahead and framed in place give the same windows and pieces under every schedule of framing steps, the end of input and quoted records around the horizon included, and so do windows the record cap ends, at the end of input or short of it; a table of records shorter than eight bytes is staged in windows of at most the cap's records; a job whose other workers finish while framing remains completes, the framing finishing later; a declined window discards the framing ahead and replays every buffered byte, with the scalar diagnostic and retained values; a refused import has read at most two windows and one transport chunk past its last accepted boundary |
| Streaming inputs | A producer that writes one transport chunk holding a malformed record and keeps its pipe open sees the error reported without a window's worth of input or EOF, with the scalar diagnostic order, retained values and no facts added; a FIFO named by path, a stream without a descriptor and forced batching are all read by the scalar consumer; a test build that takes a failing stream for a regular file shows windows staged before the failed read, so the batch path's read errors stay covered |
| Small-input cost | Tiny, empty, header-only, duplicate-heavy and one-fact inputs avoid unnecessary dispatch and keep their established behavior |

Extend `test_facts`, `test_import`, `test_value`, `test_symbol`, relation
tests and the allocation-failure suite. Review code, security and readability
before implementation tests. Run the full suite, ASan and UBSan for payload
ownership, offsets and capacity arithmetic; run TSan for the new shared
jobs and barriers. LSan and MSan run where supported. Repeated context use
and retained pointer validity are required alongside leak checks.

Benchmark serial ownership transfer first, then serial batching, then
multiple workers. Target `facts-join`, `files-grouped`, `cspa-linux`, grid
and tree inputs under the
[measurement rules](../docs/todo-benchmark-performance.md#measurement-and-acceptance-rules).
Separate input/framing, decoding, dictionary lookup, ordered numbering,
value reservation/publication, tuple construction and final commit. Record
wall time, user CPU, peak RSS, live staging and final accounted capacity.
Use new and populated contexts, repetitive and all-distinct values, and
vary worker count through 1, 4, 8, 12 and 16 on the M3 and through 1 to
64 on the p620. The default is judged against the importer before this
design, and forced batching at each worker count against the scalar
importer, with serial time, parallel time and peak memory reported
separately. `RELIC_IMPORT_BATCH_WORKERS` is the least worker count at
which forced batching beats the scalar importer on most large tables on
both machines. A table may still lose at that count; the stage's entry in
[the benchmark plan](../docs/todo-benchmark-performance.md) records the
measurements and that trade-off.

Acceptance requires lower first-load peak memory and serial load time,
then a substantial measured parallel gain on large imports without a
repeatable unexplained small-import regression. Peak instrumentation must
show one final tuple payload at an empty-store commit. End-to-end
`facts-join` acceptance also includes indexed-driver work from the ordered
plan; faster loading alone does not close that workload's query gap.
Pool-entry packing and other representation changes need their own
measurements; ownership transfer alone is not a promise that every total
RSS target is met.

## Alternatives not taken

* **Commit each parsed window as base facts.** A later malformed record
  would leave a prefix committed and violate load atomicity.
* **Intern on parsing workers.** First-arrival numbering and partial
  publication make identities and error prefixes depend on scheduling.
* **Assign values by stripe.** It changes a cross-subsystem identity
  contract and partitions the available identifier space. The ordered
  alternative retains those contracts and has an explicit measurement gate.
* **Report the first worker to fail, or sort error locations.** Completion
  order is not input order, and a displayed location is not the point at
  which the scalar reader detects the error. Replay uses the actual contract.
* **Reimplement every scalar failure in a batch prefix simulator.** Pool
  growth, type-use growth and record admission have distinct side effects.
  A sufficient whole-window proof and one scalar refusal path avoid a
  second implementation of that sequence.
* **Turn a staged set into a relation by copying its arrays at commit.**
  This retains the peak the ownership transfer is meant to remove.
* **Parallel writes into one open-addressed global dictionary.** They need
  synchronization and publication rules beyond assigning deterministic IDs.
  Private reconciliation parallelizes the expensive read/compare work first.
* **Charge staging beside the derived relations it will replace.** This
  would refuse loads that fit after discard and silently redefine the
  configured memory limit.
* **Split source programs by newline or period.** Statements, strings,
  rules, queries and recovery belong to the source parser; table framing
  does not establish a safe program-text partition.
