# Pending value compaction

This document covers value resolution at the start of a deferred round's
merge. Workers collect computed values under local markers. The merge
preserves that local deduplication, orders compact descriptors by their
first logical occurrence, and resolves them through the ordinary value
interner before comparing candidates across workers.

Related: [architecture](architecture.md#parallel-evaluation),
[computed values](design-values.md#interning-computed-values),
[ordered merge](design-parallel.md#admission-and-the-ordered-merge),
[memory](design-memory.md#round-scoped-buffers),
[testing](design-testing.md),
[implementation plan](../docs/todo-benchmark-performance.md).

## Model

A candidate column contains either an assigned domain identifier or a
pending marker. Domain identifiers have nonzero type bits. A marker has
zero type bits and a positive index into a private pending-value table;
zero is neither a value identifier nor a marker.

During derivation, a worker normalizes a batch's markers into its own
table. Each distinct pending integer or string value has one local marker
for that worker and round. An integer descriptor holds its numeric value;
a string descriptor holds an offset and length in the worker's private
byte buffer. Equality includes the value kind and the complete payload,
including embedded NUL bytes in strings.

The merge uses three mappings:

| Mapping | Index or key | Result |
| --- | --- | --- |
| Worker pending table | Worker-local marker | Integer or string descriptor |
| Local-to-round map | Worker identity and its local marker | Compact round descriptor number |
| Resolution map | Compact round descriptor number | Promised domain identifier |

A compact descriptor represents a **referenced worker-local value**, not
necessarily a globally distinct value. Equal values from different workers
can have different descriptor numbers; the existing value resolver gives
them the same promised identifier.

Candidate columns keep their local markers while descriptors are collected
and measured. One rewrite then replaces each local marker directly with
its promised identifier through the two maps. Compact round descriptor
numbers are never stored in candidate columns.

```text
workers: local markers + distinct values + owned string bytes
    -> barrier: completed candidate buffers
    -> serial task-order walk: compact descriptors and local-to-round maps
    -> ordinary value measurement: descriptor-to-identifier map
    -> parallel rewrite: local marker -> promised identifier; rehash tuple
    -> barrier: temporary compaction storage released
    -> tuple merge, admission, value append and tuple publication
```

No global value is interned during descriptor collection, measurement or
candidate rewriting. A promised identifier is private until reservation
and append succeed. The value tables' identifier-space checks remain part
of reservation, before any tuple naming a promised value is published.

## Invariants

* **One descriptor per referenced local marker.** A zero-initialized map
  slot is filled once, on that marker's first occurrence in task order.
  Later occurrences reuse it.
* **Descriptor order is logical derivation order.** The caller walks the
  task array, each task's candidate range, and each tuple's columns in
  order. Worker number and completion order do not assign identifiers.
* **Worker identity is part of a marker's meaning.** Marker 1 from two
  workers can denote different values. Only the owning worker's map can
  translate a candidate's marker.
* **Candidate columns change namespace once.** They contain local markers
  until the rewrite and promised identifiers afterward. Collection and
  measurement leave them untouched.
* **Global equality stays in the value layer.** Compaction deduplicates
  local marker references; it does not implement another string or integer
  interner.
* **Cross-worker tuple comparison uses normalized columns.** Rewriting,
  hash recomputation and stripe correction finish before any merge stripe
  list is built.
* **Private storage is charged for its lifetime.** Temporary maps and
  descriptors have an earlier release point than the resolution map and
  the string bytes it borrows.
* **A declined merge publishes nothing.** Private rewritten candidates
  can be discarded; the serial retry starts from the established store.

## Sizing the work

Let `W` be the worker count. For worker `w`, let `Vw` be its pending-table
entry count and `Sw` its used pending-string bytes. Define:

```text
U = sum(Vw)                         descriptor capacity
S = sum(Sw)                         copied string bytes
C = total candidate columns
P = pending occurrences among those columns
R = referenced worker-local values  actual compact descriptor count
G = distinct semantic values represented by those descriptors

G <= R <= min(U, P)
P <= C
```

Capacity is obtained from worker tables without first scanning candidate
columns. Only referenced entries become descriptors, so unused worker
entries are neither measured nor interned. The implementation does not
need an invariant that every allocated worker value remains referenced.
There is one ordered collection walk before measurement, with no separate
pass to count pending column occurrences.

Compute `U`, `S` and each worker's map and byte spans with checked size
arithmetic. The temporary descriptor namespace uses the existing pending
marker width, so `U` must fit `RELIC_DOM_INDEX_MASK`; exceeding it declines
the optimization. This is a workspace admission condition, distinct from
the final value tables' capacity limits. A serial retry can still succeed
when cross-worker duplication makes this capacity too large.

If `U` is zero, resolution allocates nothing and leaves the ordinary merge
to process candidates containing assigned identifiers.

## Storage ownership

### Temporary compaction storage

A compaction object local to `resolve_pending()` owns one charged block
containing:

* `U` local-to-round map entries, initially zero;
* room for `U` compact pending descriptors, with `R` entries populated.

Use the existing checked block-layout helpers for typed alignment and byte
counts. Per-worker span records identify a map slice of `Vw` entries and a
string-byte base offset. These control records are bounded by the worker
count, as the merge's other per-worker records are; data-sized storage is
charged through the round account.

The object exists only during collection, measurement and the rewrite job.
Its lifetime encloses the pool barrier that finishes rewriting. Closing
it frees the single block and releases exactly its recorded charge.

### Merge-owned resolution storage

A separate merge-owned object holds:

* the ordinary `relic_pending_map_t`, opened with room for `R` entries;
* the copied pending-string buffer;
* the projected persistent value-storage size;
* its charge and the receipt for any successful value reservation.

The object is opened after collection, and only when `R > 0`. Collection
needs only the worker descriptors, the local maps and each worker's
checked byte base, not the copied buffer, so the map can be sized by the
descriptors actually referenced. Opening charges the map and the string
buffer together, all or nothing. When `R = 0` nothing is opened, and no
resolution teardown is needed.

The string allocation contains at least one addressable byte even if
`S = 0`. An empty string can then name offset zero without forming a
pointer from `NULL`. This minimum allocation is charged. Payload content
for a zero-length string is never read.

The resolver's fresh-string records borrow bytes from this buffer until
append or reservation rollback completes. They do not borrow compact
descriptors. The descriptor array can therefore disappear after the
rewrite barrier, while the resolution map and byte buffer remain live
through the merge's reservation and apply phases.

The resolution owner records its allocations independently of the
temporary descriptor pointer. Its close routine unreserves a live
reservation when necessary, closes the value map, frees the byte buffer
and releases its charge. Partial initialization and repeated cleanup are
safe. Releasing descriptors cannot make the merge forget these resources.

## Collecting compact descriptors

All derivation tasks have completed successfully before this phase. The
caller owns the compaction object; no worker modifies candidates, pending
tables or byte buffers while it is being populated.

Assign each worker a disjoint span of the string buffer the resolution
will hold: its byte base is the sum of the earlier workers' `Sw`, which
`S` bounds. A text descriptor copied from worker `w` adds that worker's
byte base to its offset; the source descriptor remains unchanged. Validate
its offset and length against the source span before rebasing. A byte
range and its length determine a value; no C-string terminator or text
encoding is assumed.

Walk the task array in serial order. Each task identifies its worker and a
contiguous range of that worker's candidates. Walk that range in candidate
order and each candidate in head-column order:

```text
for task in task order:
    local_map = this task's worker map slice
    for candidate in the task's candidate range:
        for column in head order:
            if column already has domain type bits:
                continue
            m = column's worker-local marker
            validate m against this worker's pending table
            if local_map[m - 1] == 0:
                copy worker.pending[m - 1] into descriptors[R]
                rebase a copied text descriptor to the merge byte buffer
                R += 1
                local_map[m - 1] = R
```

This walk writes only maps and descriptors. The worker's pending table,
candidate words, hashes and stripe counts remain unchanged. The map's
one-time zero-to-positive transition establishes the descriptor bound by
construction; no second hash table or list of already-seen values is needed.

Gathering checks marker and descriptor bounds before indexing or copying.
Debug assertions also identify internal contract violations. A refused
collection follows the merge-decline path before any candidate mutation.
Because no candidate changes between collection and rewriting, each marker
the rewrite reads has a validated, nonzero mapping in the correct slice.

If `R` is zero, close the temporary storage and continue with no pending
values; no resolution storage was opened. Otherwise open the merge-owned
resolution with room for `R` entries and `max(S, 1)` string bytes, and copy
each worker's used string bytes once into its span, skipping zero-length
copies. A refusal of that allocation leaves candidates and global values
untouched. Measurement then receives exactly the `R` populated
descriptors; capacity `U` is not its input count.

## Measuring and rewriting

### Ordinary value measurement

Call `relic_pending_measure()` with the compact descriptor sequence and
the copied byte buffer. Its existing behavior is the authority for:

* reuse of an assigned integer or string identifier;
* deduplication of equal values from different workers;
* first-occurrence numbering of new values within each value table;
* reuse of a symbol pool entry as a string and its required string-use bit;
* stepwise growth of the value tables, symbol pool and use-bit storage.

The map's room and input count are both `R`; its identifier array holds
one promise per descriptor. An unrepresentable storage projection declines
the merge. Measurement does not reserve or publish values.

### One rewrite and rehash per candidate

After measurement, publish a read-only rewrite job containing the maps and
resolution state. One part owns one worker's candidate buffer; the physical
thread running that part need not be that worker's derivation thread.

For a pending column whose local marker is `m` in worker `w`:

```text
r = local_to_round[w][m - 1]
column = resolution.ids[r - 1]
```

Assigned columns are unchanged. The two indexed reads use maps already
validated and published before the job. The part allocates nothing and
cannot fail. A candidate with any rewritten column receives a recomputed
tuple hash and stripe. Correct its worker's old and new stripe counts
before recording the new stripe; when they are equal the net change is
zero. Candidates with no pending columns reuse their existing hash.

Each part writes only its worker's words, candidate metadata and stripe
counts. It reads the compaction maps, promised identifiers and target
relation metadata. No value table, relation or secondary index is changed.

The worker's candidate screen refers to its collection-time columns and
hashes. That screen is no longer consulted after derivation completes;
it is released with the candidates rather than rebuilt. Cross-worker
screening uses the normalized candidates and their recomputed hashes.

When `relic_pool_for()` returns, its barrier establishes that all parts
have finished. Close the temporary compaction object before allocating
the blocks for finding new tuples, staging or classifying keys. Later
merge phases cannot retain a pointer to that local object or its block.

## Ordered identity: example and proof

Suppose the frozen store lacks string values `amber`, `blue` and integer
`7`. Tasks in serial order produce the following two-column candidates:

| Task | Worker | Values | Local markers |
| ---: | ---: | --- | --- |
| 0 | 1 | string `amber`, integer `7` | `1, 2` |
| 1 | 0 | integer `7`, string `blue` | `1, 2` |
| 2 | 1 | string `amber`, string `blue` | `1, 3` |

Worker 0's pending table contains `7, blue`. Worker 1's contains
`amber, 7, blue`. Scanning workers by number would encounter `blue` before
`amber`, which is not serial first occurrence.

Task-order compaction produces five descriptors:

```text
1: worker 1, local 1 -> string amber
2: worker 1, local 2 -> integer 7
3: worker 0, local 1 -> integer 7
4: worker 0, local 2 -> string blue
5: worker 1, local 3 -> string blue
```

If the existing pool and integer counts are `s` and `i`, measurement
promises strings `amber` and `blue` the pool indexes `s + 1` and `s + 2`,
and integer `7` the integer index `i + 1`, under their respective type
bits. Descriptors 2 and 3 map to the same integer identifier; 4 and 5 map
to the same string identifier. Task 2's repeated `amber` needs no sixth
descriptor.

More generally, let the reference sequence be every pending occurrence
among retained candidates in task, candidate and column order. Compaction
keeps only the first occurrence of each `(worker, local marker)` pair.
Every removed occurrence is equal in kind and payload to an earlier kept
one. The earliest occurrence of any semantic value therefore survives,
and first-occurrence order among semantic values is unchanged.

The value resolver sees that ordered subsequence and uses exact typed
equality. It assigns the same identifiers and persistent capacities as
measurement of the full occurrence sequence. Rewriting through the maps
then gives every candidate the same words, hashes and stripes as that
reference. Tuple deduplication, identifiers and ordered commit consequently
have the same inputs.

The existing task-screen contract connects the retained-candidate sequence
to serial derivation: removing a stored tuple removes only values already
assigned, and a worker's repeated candidate has an earlier equal candidate.
Cross-worker tuple deduplication occurs after value resolution. It cannot
remove a value's first semantic occurrence before that value is measured.

## Admission, apply and cleanup

Resolution state remains merge-owned after candidate rewriting. The
ordinary merge finds new tuples, assigns append positions and classifies
keys. Its admission includes projected persistent value growth and every
private allocation still live at that point. Released compaction storage
is no longer charged; the resolution map and copied byte buffer still are.

`relic_pending_reserve()` runs before relation reservations. Only after it
succeeds does the resolution owner record an active reservation. A later
failure releases that reservation with `relic_pending_unreserve()` before
freeing the map or copied bytes. This rewinds uncommitted string copies;
capacities that a refused reserve retained remain accounted as the value
layer specifies.

Once every reservation succeeds, `relic_pending_append()` publishes the
values and string-use bits before tuples naming them are published. It
cannot fail. The reservation receipt is consumed, so ordinary cleanup does
not rewind committed pool data. The pool owns its copies, independently
of the merge's byte buffer.

The serial batch resolver keeps its measure/rewrite/commit contract. The
deferred merge composes its local map with the existing resolution map;
it does not manufacture a partially initialized value map to call the
serial rewrite helper.

| Owner | Responsibility |
| --- | --- |
| `lib/task.c`, candidate records in `lib/round.h` | Normalize batch markers locally; own worker pending descriptors, payload bytes and candidates until round release |
| `resolve_pending()` in `lib/merge.c` | Size and own temporary compaction storage; enumerate first references; open the resolution when `R > 0`; measure compact descriptors; wait for rewrite completion; close temporary storage |
| Merge-owned resolution object | Own the value map, copied bytes, charge, projection and reservation receipt through apply or decline |
| `lib/value.c` | Determine canonical identifiers, project persistent storage, reserve, append and unwind uncommitted string copies |
| `lib/round.c`, `lib/pool.c` | Enforce derivation/rewrite barriers, release abandoned candidate buffers and run the serial retry |

## Memory and work bounds

The data-sized resolution workspace is bounded by:

```text
bridge_bytes(U)                                             when R = 0
bridge_bytes(U) + relic_pending_map_bytes(R)
    + relic_pending_probe_bytes(R) + max(S, 1)              when R > 0
```

The merge charges the measurement probe beside the map, then releases
the probe after measurement. The map remains until append or rollback.

Here `bridge_bytes(U)` is the checked aligned layout of `U` local-map
entries and `U` pending descriptors. It is zero when `U = 0`; the whole
resolution path is then empty. Map entries use the existing 32-bit marker
width. Worker-span records add bounded `O(W)` control storage.

Temporary mapping and descriptor storage is `O(U)`, the resolution map is
`O(R)`, string storage is `O(S)`, and none is proportional to pending
occurrences `P` at fixed worker tables. Preparing maps and copying string
bytes takes `O(U + S)` work, with `O(W)` worker bookkeeping. Candidate
traversal and rewriting take `O(C)` work. Measurement receives `R`
descriptors and hashes their referenced payloads, with `R <= U`, rather
than receiving `P` descriptors. Worker-side expression evaluation and
local value hashing are separate costs and retain their existing behavior.

For the scale-8 `files-grouped` fixture, 1,799,657 `kind` tuples contain
roughly 3.6 million pending directory and extension columns. The 206
distinct string values give at most `16 * 206 = 3296` worker-local
descriptors at sixteen workers. This is a descriptor bound, not a promise
of the same factor of end-to-end speedup.

When values are all distinct, `R = U = P` and compaction saves no
descriptors. The additional local maps cost four bytes per local value
plus alignment/control storage. Releasing the temporary block before the
later merge allocations limits its overlap, but this does not by itself
guarantee a lower peak. All-distinct computed values are an acceptance
control for both memory and runtime.

## Failure paths and limits

Check count sums, typed-array byte sizes, block alignment, byte-buffer
offsets and combined charges before allocation or pointer formation.
Use `relic_pending_map_bytes()` as the authority for the value map's
allocation size. `SIZE_MAX` from a projection or an unrepresentable marker
capacity declines resolution; a truncated count never authorizes storage.

Every owned allocation is charged before it is requested. A refused
allocation closes the objects opened so far and gives back their charges.
Temporary storage has one scoped cleanup path; merge-owned resolution
storage has one close path used after both success and decline. Cleanup
is based on owned resources; when no descriptor is referenced, no
resolution storage exists to clean up.

A refusal during sizing, allocation, collection, the resolution allocation
that follows collection, or measurement leaves candidate columns
untouched. A failure in a later merge phase can find rewritten private
candidates; it discards them whole. No inverse mapping or marker
restoration is needed. The derivation and rewrite barriers must both be
complete before any storage they reference is released.

The merge reports no resource-refusal diagnostic. The established serial
retry reports the failure it encounters, preserving the existing tuple,
memory and value-limit contract. An admitted merge makes no weaker
persistent-storage check because its workspace is smaller. A round that
an occurrence-sized workspace would not fit can merge successfully.
Allocator failures retain the ordinary guarantees of consistent state and
accounted retained capacities; identical allocator failure points across
strategies are not required.

Interruption follows the existing executor and round boundaries. An
abandoned round releases this workspace before retry/reporting, and the
pool remains reusable. Compaction introduces neither asynchronous teardown
nor mutation of live global value tables.

## Determinism and validation

Descriptor numbers can vary with the assignment of tasks to workers,
because worker-local deduplication changes the compact sequence. Promised
identifiers cannot vary: the sequence preserves each semantic value's
first logical occurrence. Tests compare identifiers and persistent state,
not the scheduling-dependent private descriptor numbers.

The regression is written first and fails on the uncompacted merge. Use
the existing forced-deferred, one-worker path in `tests/unit/test_round.c`
to make candidate allocation deterministic. A program emits many distinct
tuples containing a few computed strings absent from the loaded context.
Choose a documented accounted-memory budget that accommodates candidate
and ordinary merge storage plus compact value resolution, but cannot
accommodate occurrence-sized resolution. Require that the computing round
actually merges with no abandonment: answer equality alone would allow a
silent serial fallback to pass. Establish the budget from fixture sizes
and charged capacities, not RSS or elapsed time.

| Concern | Required evidence |
| --- | --- |
| Repeated occurrences | Repetition within one tuple, across batches and across tasks retains one descriptor per referenced local marker |
| Worker identity | Equal local numbers for different values stay distinct; different local numbers for equal cross-worker values resolve identically |
| First occurrence | Interleaved task ownership, including the example above and several targets, preserves integer and string identifier order |
| Existing values | Already assigned columns remain unchanged; a string over symbol-only pool bytes reuses the pool entry and sets the string-use bit correctly |
| Byte strings | Empty strings, embedded NULs, unequal lengths, large strings and hash collisions preserve exact value equality and safe byte lifetimes |
| Empty/unused storage | No-pending rounds allocate nothing; unused local entries are not interned; `R = 0 < U` opens no resolution storage; partially referenced worker tables size the map by `R` |
| Late refusal | A limit or allocator refusal of the resolution after collection leaves candidates, value tables and relations untouched and returns every charge |
| Rehashing | A normalized candidate's hash, stripe and per-worker stripe counts agree with its promised words before cross-worker screening |
| Scope release | Temporary maps can be released at the rewrite barrier without dangling references; the copied bytes remain live through reserve, append and rollback |
| Exhaustion | The existing nth-allocation failure suite covers partial opens, copied bytes, map allocation and late relation-reservation failure after value reservation |
| Resource limits | Pure sizing tests exercise marker/count/offset boundaries without huge allocations; integration sweeps compare serial and deferred refusals and recovery |
| Concurrency | Forced deferral and 2, 4 and 16 workers give the serial identifiers, tuple order, counters and persistent storage; successful merges are required |
| Reuse | A failed query followed by a successful query on the same context and pool leaves no old maps, reservations, leaked charges or string-use bits |
| All-distinct control | Actual workspace and phase time include the added local maps and show the tradeoff when no occurrence compacts |

Extend round tests, value tests, the allocation-failure suite in
`tests/unit/test_exhaustion.c`, and string/aggregate equivalence cases.
Real rounds always reference every worker-local value and let the pool
assign tasks to workers, so `tests/unit/test_merge.c` builds rounds by hand
to fix that assignment and to hold values no candidate references.
Inspect persistent value contents and stored identifiers as the round
tests do; comparing printed strings alone would miss renumbering. Verify
that computed fixture values are absent using read-only lookup rather than
interning the expected result before the test.

Run the required code/security/readability review before implementation
tests, then the full suite and ASan, UBSan and TSan for the ownership,
size-arithmetic and shared-worker changes. LSan and MSan run on supported
Linux configurations. Review every payload `bcopy(src, dst, n)` for its
source-first argument order.

Measure `files-grouped` after loading is isolated, alongside end-to-end
comparisons, low-cardinality integers and all-distinct computed strings.
Record descriptor counts, charged peak workspace, collection/measurement/
rewrite time and process peak RSS separately. Production counters and
public ABI do not need new fields for test or benchmark instrumentation.
The optimization meets its mechanism goal when resolution storage and
measurement follow `U` and `R`, first-occurrence identity is preserved,
and the targeted merge improves without an unexplained control regression.

## Alternatives not taken

* **Concatenating worker pending tables.** Worker order is not logical
  derivation order, so it can renumber values and change subsequent storage.
* **Descriptors per tuple column.** It expands references to an already
  deduplicated value and makes resolution memory follow occurrences.
* **Global interning while workers derive.** Arrival order would assign
  identifiers and introduce shared mutation into frozen-store execution.
* **A second global value hash table in compaction.** The ordinary value
  resolver already canonicalizes typed values. Local-map compaction reduces
  its input without duplicating equality or interning policy.
* **Intermediate round markers in candidate columns.** Keeping columns
  local until the one identifier rewrite avoids another write pass and a
  third column interpretation between collection and publication.
* **Sorting values by local marker, worker or hash.** Those keys do not
  preserve global first occurrence. The ordered candidate walk already
  supplies the required order.
* **Borrowing all text directly from worker buffers.** The value API uses
  offsets into one byte span. One copy per worker-local text buffer keeps
  that contract and a clear lifetime without a new multi-buffer value API.
* **A representation switch for all-distinct workloads.** A single mapping
  rule keeps numbering and cleanup independent of cardinality heuristics;
  the uniform cost is measured explicitly on all-distinct controls.
