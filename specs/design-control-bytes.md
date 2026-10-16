# Identifier-table control bytes

A tagged identifier table uses its existing byte beside each slot to decide
whether the slot is empty or a possible match before reading the identifier.
This is a storage-level optimization shared by value interning, fact staging,
derived membership and secondary indexes. The table owns the representation;
its callers continue to supply hashes and compare candidate keys exactly.

Related: [values](design-values.md#the-identifier-table),
[membership locality](design-membership-locality.md),
[storage](design-storage.md#the-canonical-index),
[research](../docs/research.md#control-bytes-before-payload--proposed-local-adaptation),
[performance work](../docs/todo-benchmark-performance.md).

## Representation

Each tagged slot has a four-byte identifier and a one-byte control value:

| Control | Meaning |
| --- | --- |
| `0` | Empty; the identifier is also zero. |
| `1` through `255` | Occupied; a fingerprint of the hash. |

For a hash's high byte `h`, the fingerprint is `h == 0 ? 1 : h`.
Thus high bytes zero and one share a fingerprint. All other high bytes retain
their distinct fingerprints. Equality of fingerprints admits a candidate;
only the owner's complete key comparison establishes equality. There are no
deletions or tombstones.

The arrays, alignment, count-based capacity, three-quarter load ceiling and
byte accounting are unchanged by this encoding. Zero initialization makes
both allocated and caller-supplied tables empty. Untagged tables use zero
identifiers to mark emptiness and need no control array.

Two independent uniformly distributed high bytes produce the same
fingerprint with probability `258 / 65536`, compared
with `256 / 65536` for eight distinct fingerprint bits. This is a distribution
calculation, not a guarantee for a workload or an adversarial input. Above
`2^24` slots, slot selection and fingerprint bits overlap as described in
[values](design-values.md#the-identifier-table).

## One linear walk

`relic_idprobe_next()` owns the probe sequence. A tagged probe examines its
current control byte:

1. Zero ends the walk at the first empty slot, without reading its identifier.
2. A different fingerprint advances to the next slot, without reading its
   identifier.
3. A matching fingerprint reads and returns the identifier, advancing the
   cursor before returning.

The walk starts at the hash's low bits and wraps at the power-of-two capacity.
It visits the same slots in the same order as an untagged walk and terminates
at the same empty slot. A caller that resumes after rejecting a candidate
continues from the next slot. A completed miss leaves the cursor on the empty
slot. The cursor holds the table, not an array pointer; a stale cursor is not
logically valid across growth, but its old mask cannot address beyond the
larger replacement.

Placement finds its empty slot through this same cursor. It installs the
identifier and its nonzero fingerprint before returning. Rehashing uses
ordinary placement. Dropping tags frees only the control array: every
identifier retains its slot, so untagged probes find the same keys.
`relic_idtab_peek()` applies the same fingerprint mapping before returning
a home-slot candidate. Existing prefetch hints retain their meaning; they
are not evidence that a demand load or cache miss has been eliminated.

## Ownership and correctness

The table implementation is the only writer of identifier and control slots.
After each completed mutation, a control byte is zero exactly when its
identifier is zero. Initialization establishes that invariant, placement
preserves it, and growth constructs its replacement using those operations.
No interner, relation, importer or executor maintains control bytes separately.

Readers see completed tables. Frozen worker reads do not overlap publication;
an append worker owns its complete stripe until the barrier. The two stores
of placement require no new atomic or synchronization protocol.

A fingerprint collision only causes another exact comparison. It changes
neither absence, duplicate detection nor the first empty slot used for
placement. Hashes, seeds and equality remain the owners' existing operations.
Identifiers therefore keep their insertion order, and capacities, allocation
requests, charged bytes and resource refusal boundaries remain the same.
Public work counters do not count private fingerprint candidates.

Reservation remains the fallible step before placement. Failed growth leaves
the source table searchable, and abandoning a prepared replacement frees it
through ordinary table reset. There is no new allocation, retained cache,
diagnostic interface or failure path. No seeds, fingerprints, slots or probe
counts become observable through the public API.

## Scope and validation

The implementation boundary is `lib/util.h` and `lib/util.c`. Its runtime
budget is at most 100 net additional lines, with no new structure fields,
allocation owner, public option or caller-specific maintenance. Tests and
design prose are counted separately. Exceeding this boundary requires a
new design decision, not another exception at a caller.

Unit tests cover all 256 high bytes, the zero/one alias, exact equality under
collisions, wrapped clusters, repeated calls after a miss, owned and supplied
memory, growth, abandoned growth and dropping tags. An independent linear
reference checks candidates and final key membership; a fingerprint match
alone is never an expected key match. Projection and failure tests require
the same byte counts and surviving entries.

Integration validation compares complete answers, output order, value and
tuple identifiers, all public counters, final accounted storage, and exact
tuple/memory refusal behavior. Import, prepared-query reuse, computed values
and parallel publication exercise existing owners through their ordinary
interfaces. Normal, ASan, UBSan and TSan suites cover the common primitive.

Software operation counts establish how many identifier reads the source
can avoid. Assembly inspection establishes whether a native compiler retains
the conditional load. Neither establishes a cache-miss reduction or wall-time
gain. Complete-command comparisons include construction, growth, successful
lookups and publication; the [performance plan](../docs/todo-benchmark-performance.md)
states the screen and rejection criteria.

This design changes scalar control inspection within the existing linear
walk. SIMD groups, a different probe sequence, a higher load factor, changed
prefetch policy and different owner choices are separate interventions. The
representation retains the input and intermediate relations required by
ordinary evaluation; it does not claim to remove their memory cost.
