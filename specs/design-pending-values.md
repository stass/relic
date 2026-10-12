# Pending value compaction

This document defines value resolution at the publication boundary of a
deferred round. It collects one descriptor per referenced private value,
orders descriptors by logical first occurrence, and rewrites candidate
columns once to the ordinary resolver's promised domain identifiers.
Private routing identity and public value identity have separate owners.

Related: [parallel evaluation](design-parallel.md),
[bounded candidate routing](design-round-routing.md),
[computed values](design-values.md#interning-computed-values),
[memory](design-memory.md), [storage](design-storage.md),
[testing](design-testing.md).

## Model

A candidate column contains either an assigned domain identifier or a
canonical pending handle. Domain identifiers have nonzero type bits. A
pending handle has zero type bits and names an entry in a round-owned
value partition; zero is neither a value nor a handle.

[Routing](design-round-routing.md#canonicalize-pending-values-before-comparing-tuples)
normalizes producers' packet-local values into these private dictionaries.
Every distinct pending integer or text value has one canonical handle for
the round. Its descriptor contains an integer or an offset and length in
its owner's byte buffer. Equality uses kind and complete payload. Owner
handles are not public identifiers, and their allocation order has no
semantic significance.

Only a sealed round reaches compaction: production has ended, all packets
are acknowledged, and tuple owners hold globally unique target tuples.
An ordered reference vector lists those tuples by their earliest logical
derivation positions. Compaction consumes this order, not owner storage
order or packet arrival order.

| Mapping | Key | Result |
| --- | --- | --- |
| Private value directory | Canonical pending handle | Owner and local entry, with its semantic descriptor |
| Owner-to-compact map | Owner and local entry | Descriptor number at first ordered reference |
| Resolution map | Compact descriptor number | Promised domain identifier |

Candidate columns keep canonical pending handles during collection and
measurement. One rewrite composes the last two mappings to replace them
with domain promises. Compact descriptor numbers never occupy candidate
columns. Packet-to-owner normalization constructs an owned candidate;
it is outside this publication rewrite.

```text
sealed unique candidates + immutable private value owners
    -> ordered candidate/column walk: compact descriptors and owner maps
    -> ordinary value measurement: descriptor-to-identifier promises
    -> parallel candidate rewrite; recompute affected hashes and stripes
    -> barrier: release temporary maps, descriptors and routing state
    -> admission, value append and tuple publication
```

The resolver remains responsible for integer identifiers, byte-pool
sharing, symbol/string use bits and stepwise capacity projections. Neither
routing nor compaction publishes global values. A promised identifier is
private until reservation and append succeed.

## Invariants

* **One descriptor per referenced canonical handle.** Its zero-initialized
  owner-map slot is filled on first reference and reused thereafter.
* **Descriptor order is logical first occurrence.** The caller walks
  unique candidates by their minimum derivation positions, then columns
  in head order. Owner number, private handle and arrival order do not
  assign public identifiers.
* **Handle interpretation belongs to one accessor.** Partition and local
  index decoding is range-checked; compaction does not duplicate routing's
  bit layout or treat a handle as a dense global array index.
* **Owned candidate columns change namespace once.** Collection and
  measurement leave them untouched; the rewrite replaces handles directly
  with promises, followed by hash and stripe correction.
* **Semantic equality remains the value layer's.** Private canonicalization
  uses its exact operations. Compaction deduplicates references, not text
  spellings with another comparator or a second persistent interner.
* **Resolver bytes outlive resolution.** Temporary descriptor maps can be
  released at the rewrite barrier; borrowed string bytes cannot be released
  until append or reservation rollback ends.
* **A declined merge publishes nothing.** Rewritten private candidates
  need no inverse mapping: the whole attempt is released before serial retry.

## Sizing the work

Let the value partitions hold `Vp` entries and `Sp` used text bytes. Define:

```text
V = sum(Vp)                       private distinct value entries
S = sum(Sp)                       private distinct text bytes
D = unique new target tuples
C = their total columns
P = pending column occurrences
R = referenced canonical handles  compact descriptor count

R <= min(V, P)
P <= C <= RELIC_MAX_ARITY * D
```

Canonicalization gives different referenced handles different semantic
values. A successful routed round references every value it accepted:
values are taken only from outgoing rows, and dropping a duplicate tuple
retains the same values in its earliest copy. The compaction interface
nevertheless handles unused dictionary entries and `R < V`; direct tests
can construct them without weakening normal ownership.

Allocate descriptor capacity and owner-map spans from `V`, without a pass
that expands pending columns into occurrences. Collection determines `R`
in one ordered walk. Open the ordinary resolution map with room for `R`,
not `V`, and only when `R` is nonzero.

Compute `V`, `S`, map spans, aligned offsets and byte sizes with checked
arithmetic. Both compact descriptor numbering and the routing handle
namespace must be representable. A private workspace overflow declines
this attempt, separately from public value-table capacity limits, which
remain the resolver's responsibility. `V = 0` needs no compaction storage
and leaves assigned candidate words unchanged.

## Storage ownership

### Temporary compaction storage

One charged block owns the zeroed maps, one slot per value-owner entry,
and capacity for `V` compact descriptors, with `R` populated. A fixed
per-partition record gives its map offset and text-byte base. These
control records follow the fixed partition count; they are not one
record per candidate column.

The compaction object's scope encloses collection, measurement and the
rewrite barrier. Once no rewrite part can read it, closing the object
frees its block and gives back its recorded charge. It owns neither the
candidate columns nor the longer-lived resolver payload.

### Merge-owned resolution storage

A separate resolution object holds:

* the ordinary `relic_pending_map_t`, opened for `R` descriptors;
* copied text bytes, with `max(S, 1)` addressable bytes when `R > 0`;
* the projected persistent value storage and its accounted charge;
* the receipt for any live value reservation.

It opens after collection, only when `R > 0`, charging the map and text
buffer together. With no referenced handle it opens nothing, even if
private owners contain unused entries. An empty string can safely name
offset zero in the addressable byte without reading a payload byte or
forming a pointer from `NULL`.

Each value owner's used bytes are copied once into a disjoint span of
that buffer. They already represent globally distinct private values,
so this copy has no worker-replication term. Descriptors reference the
copied buffer, and the ordinary resolver may continue borrowing those
bytes after descriptor collection and rewriting end.

Cleanup follows owned resources rather than the final descriptor count.
A partial open frees what it obtained. A live reservation is unreserved
before closing its map and freeing the payload. Successful append ends
the reservation; subsequent cleanup releases private workspace only.

## Collecting compact descriptors

The caller traverses the ordered unique-candidate vector. Each reference
selects an immutable owner block, tuple entry and target schema. For each
column:

1. Leave an assigned domain identifier out of compaction.
2. Decode a pending handle through the round's value accessor and validate
   its owner, entry and payload range.
3. If that entry already has a compact number, reuse it.
4. Otherwise append its descriptor, record the new positive compact number,
   and rebase a text offset to its owner's assigned span in the copied
   payload buffer.

All range and rebasing checks precede writes. No cast makes an unvalidated
marker into an array index. Candidate words remain unchanged. Only the
map/descriptors mutate, and no worker runs a producer while this walk
borrows private owner data.

If `R = 0`, close the temporary block and continue with assigned values.
Otherwise open the resolution object, copy the used text spans, and pass
exactly the populated `R` descriptors to value measurement. Failure of
that allocation leaves candidates and global values untouched.

## Measuring and rewriting

### Ordinary value measurement

`relic_value_measure_pending()` consumes the compact descriptors in their
first-occurrence order. Its existing rules govern existing values,
integer numbering, pool-byte reuse, string-use bits, identifier exhaustion
and stepwise growth of persistent tables. Measurement promises IDs but
reserves and appends nothing.

The private canonical namespace is injective by construction: one value
owner and one exact dictionary entry per typed value, with no pending
value already assigned in the frozen context. Tuple-owner uniqueness
therefore survives mapping canonical handles to domain promises. The
resolver is not a late opportunity to repair an invalid tuple-owner
canonicalization or silently collapse tuples after append positions have
been assigned.

### One rewrite and rehash per candidate

A rewrite job partitions immutable candidate blocks into disjoint spans.
Every candidate has one writer. A pending column names a validated owner
map entry, which names a populated resolver promise; those two lookups
replace the column directly. No intermediate descriptor number is stored
in the tuple.

For a tuple with rewritten columns, compute the ordinary relation wide
hash and canonical stripe from the promised words. A routing hash over
semantic pending payload is not that stored-tuple hash. Fully assigned
candidates can retain their ordinary hashes. Secondary key hashes are
computed from normalized words during staging as usual.

The rewrite barrier precedes append planning that reads hashes or stripes.
Only after that barrier can temporary maps and descriptors disappear.
Retained private value entries and payload can then be released once no
other finalization reader needs them: the resolution object owns the
bytes its map borrows. Routing membership tables end earlier, at sealing.
Candidate columns remain live until staging takes them.

## Ordered identity: example and proof

Suppose a later task's `p(1, "violet")` arrives before an earlier task's
copy, and `p(2, "amber")` arrives between them. Tuple ownership retains
one copy of each and moves the first tuple's position to its earlier
witness. Finalization visits `p(1, "violet")` first even if the private
value dictionary encountered `"amber"` first. Compaction therefore presents
`"violet"` first to the ordinary resolver. Reusing the value in another
target adds no descriptor and changes no identifier.

Every newly computed head value is absent from the frozen value tables,
so a tuple containing it is absent from the frozen relation. Removing
copies of a duplicate tuple keeps an earlier occurrence of exactly the
same values in exactly the same columns. Replacing all copies by their
minimum-position tuple preserves the minimum tuple/column position for
each value. The ordered unique stream therefore has the same semantic
first-value sequence as the original derivations.

The resolver assigns IDs to that sequence in ordinary order. Private
owner order, worker count, wave size and packet numbering cannot affect
the result. Values computed only inside expressions and never emitted
are excluded, as in ordinary execution.

## Admission, apply and cleanup

The value projection joins exact target growth and the serial executor
scratch floor in the ordinary merge admission question. All live private
workspace stays charged while that question is asked. Output-byte limits
can shorten routed batches, so the round supplies the conservative
pending-buffer floor defined by
[bounded routing](design-round-routing.md#preserve-the-serial-refusal-contract),
not merely its observed transport peak.

Value and target reservation finish before apply. A refusal gives back
uncommitted value reservations and all private work, then the selected
round reruns serially. A public reservation that grew an array before an
allocator refusal retains and accounts that capacity under the ordinary
storage contract.

Apply appends values before tuples naming them. Candidate IDs are already
promises backed by that reservation, and no dictionary lookup can assign
another ID on a worker. The subsequent relation/index append is infallible.
Resolver bytes are released only after append or rollback completes.

## Memory and work bounds

Compaction uses:

```text
bridge_bytes(V)                                            when R = 0
bridge_bytes(V) + relic_pending_map_bytes(R)
    + relic_pending_probe_bytes(R) + max(S, 1)              when R > 0
```

The merge charges the probe beside the map while it measures and
releases the probe's allocation and charge once measurement ends; the map
lives until append or rollback.

`bridge_bytes(V)` is the checked aligned layout of maps and compact
metadata; it is zero when `V = 0`. Per-partition records have fixed size.
The bound is `O(V + S)`, with no descriptor or copied-payload term per
candidate column or per worker. Candidate traversal and rewriting cost
`O(C)`; map initialization and payload copying cost `O(V + S)`. Value
measurement additionally hashes the referenced semantic payloads.

These are publication costs. Packets have their own fixed total budget,
and private value/tuple ownership has the separate
[routing bound](design-round-routing.md#memory-bounds-and-admission).
Ordering references and append staging remain visible allocations; compact
resolution is not a claim that the entire round uses only value-sized
memory.

## Failure paths and limits

Sizing, allocation, collection and measurement can decline before any
candidate changes. A later failure can find rewritten private candidates;
they are discarded rather than restored. Both rewrite and routing barriers
complete before any referenced storage is released.

Integer sums of capacities, aligned offsets, owner spans, byte lengths
and descriptor numbers are checked before allocation or access. Handles
must denote a live entry, map entries must be populated before rewriting,
and promises must have valid domain type bits. Internal malformed input
never reaches a relation, diagnostic value or caller callback.

No private failure is reported as a new user-visible resource limit.
Serial retry establishes the original refusal and diagnostic. Memory
charges track allocations at capacity through partial initialization and
cleanup, including the addressable byte for empty text. Unused entries
are not interned merely because their table allocated room.

## Determinism and validation

Tests compare exact stored IDs, domain kinds, pool use bits, candidate
words, final hashes/stripes and charges, not just printed strings.
Required cases include:

* different packet markers for equal values and equal markers for different
  values, canonicalized through real owners before compaction;
* late earlier tuple witnesses, multiple targets and repeated columns;
* mixed integers and text, existing pool bytes, empty strings and NULs;
* no-pending rounds, unused owner entries, `R = 0 < V`, and `R < V`;
* dictionary and compact-number limits, bad handles and byte ranges;
* late resolution allocation refusal, partial value reservation, rewrite
  barrier lifetime and every cleanup path;
* all-distinct values as well as many tuple references to a small vocabulary.

Keep direct compaction/merge fixtures for states normal production cannot
create. Full route/evaluator tests separately prove that owner handles
are canonical and tuple minima preserve ordering. Allocation injection,
ASan, UBSan and TSan cover ownership, arithmetic and shared barriers;
standalone LSan/MSan run where supported.

Performance counters bound descriptors by referenced handles and copies
by owned value bytes. Timing comparisons include all-distinct inputs so
that reference-map and ownership costs remain visible. Neither a clock
threshold nor a small answer alone proves compact allocation.

## Alternatives not taken

* **Descriptors per pending column.** A repeated vocabulary would regain
  occurrence-sized memory. Maps are sized by private value entries.
* **Use private handles as final domain identifiers.** Their order follows
  private dictionary arrival, and their encoding is a routing detail.
* **Rewrite through descriptor numbers first.** It adds another full tuple
  pass and temporarily makes columns use a third retained namespace.
* **Borrow a resumable producer's bytes.** A drained packet is reusable;
  resolver payload must have its own lifetime through append or rollback.
* **Keep dictionaries just to serve resolver borrows.** One compact copied
  payload separates that lifetime from routing hashes and owner tables.
* **Intern values while collecting references.** Publication must follow
  complete reservation so a declined deferred round changes no identity.
