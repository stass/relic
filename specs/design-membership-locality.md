# Canonical membership locality

A relation's canonical index maps its complete tuple to an insertion-order
identifier. Derived relations use the identifier table's existing one-byte
hash tags to avoid comparing unrelated tuples along a probe sequence. Base
stores and query-answer collectors keep compact slots. The choice is made
when an empty payload is constructed and persists through growth and clear.
There is one probing implementation and no adaptive representation switch.

Related: [architecture: storage](architecture.md#storage),
[storage](design-storage.md#the-canonical-index),
[identifier tables](design-values.md#the-identifier-table),
[hashing](design-values.md#the-hash-function-and-the-seed),
[memory](design-memory.md#the-memory-limit),
[bulk loading](design-bulk-load.md), and [testing](design-testing.md).

## Ownership and representation

The relation payload owns its packed domain columns and canonical stripes.
Each stripe is a `relic_idtab_t`: linear probing over four-byte tuple IDs,
with a separate tag byte per slot when tagged. Tuple ID zero denotes an
empty slot. Every eight-bit tag value is valid; emptiness comes from the ID.

| Owner | Canonical tags | Reason |
| --- | --- | --- |
| Derived relation | Retained | Repeated evaluation probes avoid unrelated tuple reads. |
| Base store | None | Persistent input storage stays compact. |
| Query-answer collector | None | Result storage keeps the compact representation. |
| Private fact builder | Retained while loading | New input rows cause many unsuccessful probes. |

All owners share the payload and table operations. An empty base store adopts
a builder's payload, dropping staging tags without moving columns or IDs. Appending to an existing base store uses that store's
compact tables. Rule evaluation reaches the derived table through its one
construction funnel, so a rule cannot silently write an untagged base store.
No planner flag, worker count or allocation address selects the layout.

## Exact lookup and identity

The unkeyed high part of a wide tuple hash selects the fixed stripe. The
keyed low 32 bits select the home slot and supply its top eight bits as
the tag. A probe advances linearly, wrapping at the power-of-two capacity,
until an empty ID proves absence. A mismatching tag skips the tuple read.
A matching tag only admits the candidate to exact domain-word equality.
Collisions cannot create a match, including for zero-column tuples.

The load ceiling is three quarters. Count-based growth keeps an empty slot,
so the probe terminates even when all hashes collide. At capacities above
`2^24`, slot and tag bits overlap and filtering weakens; correctness and
capacity remain unchanged. Keyed placement and the ordinary entropy-failure
contract remain in force. No seed, hash, slot or probe count becomes public.

IDs come from insertion order, not from slots. Rehashing changes neither the
column array, tuple IDs, delta boundaries nor secondary-index chains.
Successful evaluations with sufficient resources preserve value interning,
iteration order, semantic counters and diagnostics. The reported storage
bytes include the tags' actual capacity.

## Growth and concurrency

Reservation precedes column storage, canonical placement and secondary-index
linking. It may fail; placement after a complete reservation cannot. The
existing table projector and allocator both read the table's tagged kind,
so no relation-specific byte formula is duplicated at callers.

A prepared growth owns an empty replacement until completion. The old table
remains searchable. Completion rehashes existing IDs from immutable columns,
installs the replacement and releases the old allocation without allocating.
Abandonment frees only the replacement. Reset releases slots and tags while
preserving the table's kind for a later refill.

Frozen readers do not overlap placement or growth. During a parallel append,
one part owns an entire stripe and the pool barrier publishes completed work.
No per-slot atomic, concurrent insertion protocol or new lifetime is added.
Pending markers are resolved to final domain IDs before candidate tuples are
staged and their hashes enter a persistent relation.

Batch lookup retains its existing bounded prefetch hints. A hint saves no
slot or tuple pointer across insertion or growth. The real probe always uses
the current table and exact comparison. Tags introduce no candidate cache or
additional relation-sized buffer.

## Failure paths and limits

Let `C_s` be the allocated capacity of derived canonical stripe `s`. Tags
add exactly `sum(C_s)` bytes to its four-byte ID arrays: 25% of those arrays,
not 25% of the relation or process. Columns, secondary indexes and other
owners retain their own accounting. Ordinary growth may temporarily hold an
old and new table; prepared replacements held across a phase are charged
beside their sources until completion or abandonment.

Canonical sizes, capacity multiplication and growth use the identifier table's
checked arithmetic. Failed allocation leaves the source searchable; any
retained reservation capacity stays charged. Refused deferred preparation
releases its private allocations and uses ordinary serial retry. Publication
and interrupt boundaries remain those of the existing insertion/append.

A fixed memory budget can admit fewer derived tuples than compact slots
would. Within the selected owner policy, unkeyed stripes and count-based
capacities preserve admission across seeds, batch boundaries and worker
counts. No low-memory path silently substitutes a cheaper representation.

## Validation

Generic identifier-table tests cover tagged and untagged probes, wrapped
clusters, matching fingerprints, full-hash collisions, growth and projection.
Relation tests establish exact IDs, duplicates, accounting and clear/refill
through the real derived-construction path. Load/adoption, query reuse,
allocation failure and deferred-round tests exercise those operations through
the ordinary interfaces. Limits compare worker counts within this owner
policy, not against the cheaper compact reference's threshold.

Performance evidence separates software comparison counts, sampled CPU work,
complete-command timings, charged storage and process RSS. Production timing
binaries contain no reporter. Closure, same-generation, sparse, wide-tuple,
large-storage and import/startup controls make both the saved work and the
unconditional storage cost visible.

## Adoption rule

A further representation change must identify material work that the existing
tags leave, demonstrate its removal, and improve complete commands beyond
measurement and code-placement uncertainty. Report extra capacity, growth
peaks, memory-limit admission and ownership complexity together. Preserve
exact answers and failure behavior; an unexplained reproducible control
regression blocks adoption. The
[measurement protocol](../docs/todo-performance-measurements.md) defines the
comparison; the [performance direction](../docs/todo-benchmark-performance.md)
sets the boundary for further proposals.

## Alternatives not taken

### Grouped tags

Grouped control bytes require another probe sequence, group-termination proof
and layout-specific growth/bounds rules. Existing linear tags supply useful
filtering without that mechanism. A grouped design needs a demonstrated
complete-workload benefit beyond the current table before it earns an
implementation.

### Cached hashes

A full keyed hash beside every tuple ID doubles the compact slot array and
can avoid rehashing columns during growth. Growth still scans and places
entries and initializes replacement storage. The measured closure cost does
not justify that width when existing tags already remove unrelated equality
work. High-arity or different growth evidence can reopen the question without
adding an unused container to the engine.
