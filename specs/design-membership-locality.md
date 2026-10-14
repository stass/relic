# Canonical membership locality

This document defines the storage experiments and adoption contract for a
relation's whole-tuple membership index. It compares compact filtering of
probe candidates with retaining the keyed hash needed during growth. The
choice rests on complete workload measurements, including construction,
duplicate checks and growth peaks. Tuple storage, identifiers, insertion
order and exact equality remain the foundation shared by every candidate.

Related: [architecture: storage](architecture.md#storage),
[storage](design-storage.md#the-canonical-index),
[identifier tables](design-values.md#the-identifier-table),
[hashing](design-values.md#the-hash-function-and-the-seed),
[memory](design-memory.md#the-memory-limit),
[bulk loading](design-bulk-load.md),
[benchmarks](design-bench.md), [testing](design-testing.md),
[implementation plan](../docs/todo-benchmark-performance.md#use-existing-mechanisms-first).

## Model

A **canonical membership index** maps the complete tuple's domain words
to its persistent tuple identifier. The relation payload owns both its
packed columns and this index. A private fact builder owns the same kind
of payload, so an empty base relation can adopt it without conversion.
The payload remains the only owner of these allocations.

The wide tuple hash has two separate purposes. Its unkeyed high part
selects a fixed **stripe**; its keyed low 32 bits select a location inside
that stripe. The experiment changes the stripe's representation. It
changes neither the hash functions nor stripe selection. The configured
worker count determines who fills a stripe, never which stripe owns a
tuple or how much capacity it receives.

There are four comparison layouts:

| Layout | Representation within one stripe | Question it isolates |
| --- | --- | --- |
| Identifier reference | Linear probing; four-byte tuple IDs | What does the compact reference cost after other optimizations? |
| Linear tags | The same probe sequence with a separate eight-bit tag per slot | Does filtering unrelated tuples pay without a new probing algorithm? |
| Grouped tags | Groups of sixteen control bytes and sixteen tuple IDs | Does inspecting nearby metadata before loading IDs reduce dependent work? |
| Cached hashes | Linear slots containing a tuple ID and its full keyed 32-bit hash | Do stronger filtering and growth without column rehashing justify wider slots? |

These are controlled experiments, not four production modes. A build
contains one selected canonical layout. Secondary-key tables, value
interning, distinct-count tables and private round-owner dictionaries
keep their own representations. The comparison does not make every
identifier-table owner pay for a relation-specific result.

The identifier reference is the selection's fallback. A result retaining
it is complete when the measurements reject the additional mechanisms.
The design gives no layout a performance win by assumption.

## Invariants

* **A slot names a tuple; it does not define its identity.** Tuple IDs
  come from insertion order. Moving a slot moves no columns and changes
  no ID, delta boundary, secondary chain or iteration order.
* **Filtering proves inequality only.** A matching tag or full hash
  passes a candidate to the ordinary domain-word equality loop. Only
  that loop establishes equality, including at arity zero.
* **Probe and placement share one sequence.** A tuple occupies a place
  its lookup visits before lookup can establish absence. Every table
  retains empty capacity and has no deletion or tombstones.
* **Capacity depends on counts.** Each stripe uses the same minimum,
  power-of-two growth and load ceiling in all comparisons. Probe length,
  timing, seed, worker count and allocation address cannot trigger growth
  or a representation switch.
* **Prepared growth is private.** Allocation can fail before any new
  tuple becomes visible. Moving existing entries, installing a prepared
  stripe and placing reserved IDs cannot fail.
* **Readers see an immutable table.** A frozen membership screen does
  not interleave with a placement or growth. During parallel append one
  part owns each entire stripe; a probe is consumed before any mutation
  of its table.
* **Every data-sized allocation has one owner and a pure projection.**
  Control bytes, cached hashes, padding and prepared replacements count
  as storage according to their actual lifetime.

## Establishing the remaining cost

The measurement baseline uses bounded recursive plan selection, ordinary
materialized aggregates, current loading and leading-scan partitioning.
Every experiment names its exact source and build so that
a result on one baseline does not establish a default for another.

Worker-local screens remove repeated candidates within each worker, but
copies can remain across workers. Frozen-store screens can ask the same
relation about many derivations. Count at the canonical boundary rather
than inferring probe traffic from final relation cardinality or emitted
heads; a low rate of committed insertion does not imply few probes.

Separate input construction, frozen screening, ordinary insertion and
reserved append/growth in diagnostic runs. For each relation/arity,
stripe and capacity interval, collect:

| Measurement | Interpretation |
| --- | --- |
| Successful and unsuccessful membership calls | Duplicate-heavy and new-key work have different dependent reads |
| Probes or groups visited, with distributions and maxima | Means alone hide collision tails and wraparound |
| Occupied candidates, filter matches and exact equality calls | Tags help only when they avoid work the reference performs |
| Domain words compared, stopping at the first differing word | A high-arity mismatch need not read a whole tuple |
| Placement walks, separately from membership | A known-new insert still finds a free position |
| Growth count, entries moved and words rehashed | Cached hashes target this cost in addition to lookups |
| Bytes initialized, read and written during growth | Removing hash arithmetic can leave allocation/zeroing dominant |
| Per-stripe counts, capacities and charged peaks | Uneven stripes and retained replacements affect total memory |

Counters describe software operations, not hardware cache misses. Sampling
and available hardware counters provide separate evidence for dependent
loads, instructions, branches and bandwidth. An inclusive sample below
the insertion function also includes hashing, secondary-key work and
reservation; it does not attribute all that time to canonical lookup.
Let `f` be the measured fraction of total time in the operation a change
actually removes: even eliminating it completely saves at most `f` of
that run. A faster probe microbenchmark cannot override this bound.

Instrumentation is confined to private diagnostic builds and synthetic
fixtures. Parallel counters belong to a task or stripe owner and are
combined at a barrier; no shared counter or atomic increment enters the
timed hot loop. Production statistics, explain output and diagnostics
expose no seed, hash, slot, probe count or tuple payload. Timed production
binaries contain no diagnostic collection or dump path.

## The comparison layouts

### Common capacity and hashing

For one stripe let `n` be its occupied count and `C` its capacity. Empty
storage has capacity zero. The first allocation has sixteen slots;
nonzero capacities are powers of two. Room for `a` additions means
`n + a <= C - C / 4`, with the sum checked before use. A growth chooses
the smallest larger capacity reached by doubling that satisfies this
condition. A batch reservation reaches the same capacity as the
corresponding single insertions.

These rules are the identifier table's existing capacity policy. Keep
the load ceiling and minimum identical while comparing representations;
raising the load factor is a different experiment. Under occupied-count
limits the table always contains an empty slot. A finite probe sequence
that covers the table therefore reaches either a match or emptiness.

The whole tuple is hashed once for an ordinary membership request.
Prepared hashes from loads and relation screens are reused. Debug checks
verify them against the tuple, arity and context seed. Hash zero is an
ordinary hash; only tuple ID zero denotes no tuple.

### Identifier reference and linear-tag control

The reference uses `relic_idtab_t` with no tags. Its home slot is
`h & (C - 1)`, followed by increasing slot numbers modulo `C`. Every
occupied ID is an equality candidate. The first empty ID proves absence.
Known-new placement follows the same sequence to its first empty slot.

The linear-tag control uses the existing tagged identifier table, with
the top eight bits of `h` beside each slot. A mismatching tag skips
equality, while the empty ID still terminates the search. All eight-bit
tag values are valid. At capacities above `2^24`, slot and tag bits
overlap; filtering can weaken without affecting correctness.

This control matters even if grouped metadata is the preferred
experiment. If one constructor argument provides the useful improvement,
the result does not justify a new table implementation.

### Grouped tags

A group contains sixteen one-byte controls followed by sixteen
four-byte IDs. Groups form one allocation. Their logical size is eighty
bytes; the implementation checks the actual layout and accounts for any
padding. Ordinary alignment sufficient for a `uint32_t` is enough. The
design does not assume a particular cache-line size or claim that a group
fits in one line.

Control zero means empty. An occupied control is
`0x80 | ((h >> 25) & 0x7f)`: the high bit marks occupancy and the low
seven bits carry a fingerprint. The other control encodings are unused.
Slots and controls are initialized, so probing never reads indeterminate
padding or an uninitialized ID. Occupied controls name nonzero IDs;
empty controls have zero IDs.

For `G = C / 16` groups, lookup begins at `h & (G - 1)` and advances
through groups modulo `G`. It does not select an initial lane within the
group. One pass over the sixteen controls produces an exact match mask
and an empty mask. Lookup visits matching lanes in increasing lane order,
fetches their IDs, and asks the relation to compare the tuples. After
all matching lanes in that group fail equality, any empty lane proves
absence; otherwise lookup advances to the next group.

```text
group = h & (group_count - 1)
repeat:
    matches, empty = inspect_controls(group, fingerprint(h))
    for each set lane in matches, in increasing order:
        if equal(columns_of(group.ids[lane]), query):
            return group.ids[lane]
    if empty is nonzero:
        return no_tuple
    group = (group + 1) & (group_count - 1)
```

In particular, an empty lane does not let lookup skip a matching lane
in the same group. Group-level termination follows from insertion's
rule: a known-new tuple uses the lowest empty lane in the first group
with space on that same walk. If a tuple lies in a later group, every
earlier group was full when it was inserted and remains full until reset
or replacement. Within its own group lookup examines every matching
control. This proves absence without an overflow bitmap, displacement
counter or tombstone protocol.

The group count is a power of two, so this walk visits every group.
The load ceiling guarantees at least one empty lane somewhere. Even if
all hashes and fingerprints collide, lookup terminates and exact
equality distinguishes the tuples. Seven-bit filtering is a performance
property only; neither a uniform distribution nor a `1/128` rejection
model is a correctness assumption. Above `2^25` groups (`2^29` slots),
home and fingerprint bits overlap, which the measurements treat
explicitly.

Placement writes the ID and its occupied control within the exclusive
writer phase, then updates the count. There is no concurrent-reader
publication protocol hidden in the control byte. Growth reinserts IDs
using the hash recomputed from the owning payload's columns. A tag is
insufficient to reconstruct that hash.

The portable implementation first computes masks with a clear bounded
lane loop. A vector implementation, if justified, changes only this
helper. It loads exactly the sixteen addressable control bytes; it
requires no cloned tail, sentinel beyond the allocation, cache-aligned
allocator or speculative read past the final group. Masks use lane zero
for the lowest address on every endian and CPU variant. Vector and
portable helpers produce identical masks, including for all-empty and
all-matching controls. Word tricks that identify whether a byte matches
are insufficient if borrow propagation produces an inexact lane mask.

CPU instructions are selected through the existing build portability
mechanism, with a tested portable implementation and no dependency.
Neither group width nor storage size changes with runtime CPU dispatch.
Vectorization and prefetch tuning are measured separately from the
representation, so a gain is attributable to a specific change.

### Cached hashes

Each linear slot contains a four-byte ID and the keyed low 32 bits of the
tuple hash. Its logical size is eight bytes, without a pointer or copied
column. An empty ID terminates the probe; its hash is ignored. A differing
hash skips the tuple read. An equal hash still requires exact equality.
The home slot, walk, load ceiling and first-empty placement match the
identifier reference.

During growth the source slot supplies both ID and hash. No callback
reads tuple columns or hashes them again. The stripe is already known,
so keeping the unkeyed high part of the wide hash buys nothing here.
New entries obtain their cached hash from the same prepared hash used
for membership and placement. Neither rehashing nor copying silently
changes its seed or mixes in a pointer, tuple ID or thread number.

The cache applies only to final domain columns. Hashes of private pending
markers never enter it. After pending values resolve,
the ordinary final hash and stripe are computed before persistent append.
The cache stays correct by construction because stored columns are
immutable for the payload's lifetime. Reset discards entries; moving a
payload between a builder and its base relation keeps the context seed.

This layout pays twice the reference's slot bytes on every occupied and
unused slot. It must earn that cost on complete runs, including input
construction and steady successful membership, not just on a captured
growth. The comparison records tuple-column reads eliminated during
growth and the wider array's initialization and copying traffic.

## Storage boundary and lifetime

### A canonical-specific implementation boundary

The payload exposes membership through candidate iteration, reservation,
placement, growth completion, byte projection and bounded prefetch hints.
The relation owns tuple equality. No executor, importer or merge part
reads a control byte, assumes a slot width or records a slot address.
Bulk loading and evaluation reach the same payload operations.

For a selected grouped or cached-hash layout, a small internal
`lib/member.h` / `lib/member.c` owns the canonical stripe and its probe.
It supplies the equivalents of the identifier table's existing
`init/reset`, `probe_open/next`, `has_room`, `reserve/place`,
`prepare_growth/complete_growth` and byte-projection contracts. These
operations use a canonical-specific type, so direct use of generic slot
arrays fails to compile. Shared checked capacity arithmetic has one
definition; each representation owns its allocation layout and probe
sequence. The result is a focused storage type, not a generic container
with a key callback, runtime method table and collection of mode flags.

If the linear-tag control alone wins, the existing identifier table
already provides the mechanism. No new module or abstraction is required
just to change that construction choice. Experimental implementations
are selected at build time in isolated builds, not by a public flag or
an environment variable read in the library.

### Reservation and growth

Ordinary insertion first establishes absence and classifies secondary
keys. Projection and reservation precede storing columns, placing the
canonical ID and linking secondary indexes. A reservation can leave
larger retained capacity after another allocation fails; reconciliation
charges what remains, while contents and all indexes still agree.

Prepared growth allocates and initializes an empty replacement stripe
without modifying its source. The source remains valid until completion.
Completion moves all occupied entries, installs the replacement and
releases the old block; it allocates nothing. Closing abandoned prepared
growth frees only the replacement. Its type and ownership do not permit
a half-filled replacement to become the live table.

Rehashing may walk old slots in physical order because it assigns no
identifiers and publishes no iteration order. Grouped and linear-tag
layouts invoke the payload's keyed rehash for each occupied entry;
cached-hash layout copies the stored hash. Debug and test builds verify
the latter against the columns in a separate validation pass, excluded
from required-growth counters and timed production builds. Completing
one stripe needs no list of all relation tuples and touches no other
stripe's storage.

Each stripe retains its ordinary count and capacity. The prepared append
counts additions by their final canonical stripe before reserving.
Parallel parts complete and fill distinct stripes; the coordinator
publishes relation counts and frontiers at the ordinary barrier. There
are no atomics per slot, concurrent group insertion, or writes shared by
two stripe owners.

An empty base relation adopts the builder's complete payload, with one
ownership/accounting transfer. A populated destination appends with
prepared hashes. Layout changes introduce no conversion, duplicate
membership pass or second payload at that boundary. Rollback, clearing,
query destruction and context destruction all use the payload's reset.

### Batch lookup and prefetch

Keep the existing separation between hashing/prefetching, fetching a
likely candidate and consuming the batch in order. An initial hint names
the reference slot, cached-hash slot or grouped control block. A bounded
second hint chooses at most one plausible ID from that home slot/group
and immediately prefetches its tuple columns. A grouped hint can reject
nonmatching tags before reading IDs; a cached-hash hint can reject a
differing hash. Arity-zero tuples require no column prefetch.

Hints are optional and return neither membership nor a saved empty slot.
An intervening insertion may grow columns or the index. The subsequent
real probe reads the current table and recomputes its masks and position.
No batch retains a slot pointer or a candidate's column pointer across
mutation. A no-prefetch build executes the same equality and insertion
path and produces the same results.

Do not add unbounded candidate prefetch lists, a per-probe allocation or
another relation-sized cache. Group masking, local probe state and
bounded batch arrays are sufficient. Measure variants with their ordinary
hints and with hints disabled; retuning the batch size is a separate
experiment from changing slot layout.

## Failure paths and limits

Let `C_s` be each stripe's allocated capacity and `P` the column array's
charged bytes. Ignoring unchanged secondary indexes, the logical steady
storage is:

| Layout | Payload storage |
| --- | --- |
| Identifier reference | `P + 4 * sum(C_s)` |
| Linear tags | `P + 5 * sum(C_s)` |
| Grouped tags | `P + 80 * sum(C_s / 16)` |
| Cached hashes | `P + 8 * sum(C_s)` |

These formulas describe the specified field sizes. Implementation
projectors use the actual checked layout, including padding and any
alignment slack requested from the allocator. Metadata is not omitted
because it lives beside slots or inside a helper object. Fixed payload
headers keep the ordinary program-sized accounting classification.

For a normal doubling, the canonical allocation transient is the old
`C` slots plus the new `2*C`, or `3*C` slots of that representation.
It is fifty percent above the new array alone. This is a byte/lifetime
calculation, not a prediction of process RSS; page residency, allocator
retention, columns and other arrays also contribute.

The existing memory contract distinguishes two lifetimes. The temporary
overlap while an ordinary growth copies and immediately releases its
source is an allocator transient, measured for peak evaluation but not
an extra held-capacity charge. A prepared replacement retained across
a phase is held storage and is charged beside the old table. If `C'_s`
is the capacity after a reserved append, its storage projection includes:

```text
projected column storage
    + sum(bytes(C'_s))
    + sum(bytes(C_s) for stripes with prepared replacements)
    + secondary-index storage and its prepared replacements
```

Append records, candidate owners, resolver state and other live scratch
remain in their own charged accounts. The same pure layout and growth
helpers drive projection and allocation. Release or installation of a
prepared table reconciles the actual owners; no caller guesses a credit
from occupied count or assumes every stripe doubles at the same time.

Check addition, doubling, multiplication and alignment rounding before
allocation. An impossible capacity is an ordinary allocation failure,
not a wrapped small request or fabricated memory-limit diagnostic.
The identifier table's projector returns the current bytes when the
requested capacity is unrepresentable, and reservation then fails; the
canonical equivalent preserves that distinction. Failed allocation
leaves the source searchable and the replacement empty/resettable.
Partially reserved relation capacity remains charged. Deferred refusal
releases private preparation and uses the ordinary serial retry and
diagnostic path.

Interrupts use the ordinary evaluation boundaries before or after a
batch/append operation. No new interruption return splits an infallible
placement or leaves a partly moved stripe live. Prepared work can be
abandoned before completion; once reserved publication starts it finishes
with the store consistent before the caller observes interruption.

Larger slots can make a lower memory limit refuse an insertion the
reference layout admits. Report that cost explicitly. Equivalence of
limit decisions is required across seeds, batches and thread counts
*within one selected layout*. It is not a claim that different storage
representations consume identical bytes. Every candidate uses its real
storage in serial and deferred admission, including the serial scratch
floor; no low-memory mode switches to a hidden cheaper layout.

A membership probe borrows a table only while it is immutable. Growth
and reset require exclusive ownership, so a cursor cannot span either.
All masks and lane numbers are bounded by the real allocation; bit-scan
operations receive a nonzero mask and shifts stay within the unsigned
operand's width. Word equality uses domain values, not struct padding or
unaligned casts. At arity zero the equality loop and hash consume no
column, and no new helper forms a pointer from a null allocation merely
to perform a zero-length read or copy.

Hash collisions may cause long walks but cannot forge membership,
expose uninitialized data or force a seed-dependent growth. The existing
keyed placement and entropy failure contract remain in force. Tests can
inject collisions under a private test seed; production supplies no
fixed-seed switch or probe oracle. A cached hash is private storage,
never a value or a serialized field.

## Determinism

For a payload with the same tuple sequence, all four candidates return
the same member ID: exact equality selects at most one stored tuple and
the probe/placement invariant makes that tuple reachable. An absent
tuple is appended once after successful reservation. Induction over
input order therefore gives identical IDs, columns, deltas and secondary
chains. Rehashing changes none of those objects.

The unkeyed stripe function gives the same per-stripe counts under every
seed. The common count-based capacity rule gives the same capacities
within each layout, regardless of batch boundaries or which worker
places the entries. Probe order and slot contents can differ; no
reported counter or output depends on them. Storage-byte statistics
reflect the chosen layout's actual capacity.

This design introduces no new planner estimate, runtime cardinality
catalog, derived/base tie or adaptive policy. Canonical membership
remains an implementation of the store contract. The recursive selector
continues to read secondary-key counts directly through const inputs.

## Validation and selection

### Correctness and failure evidence

A small reference set independent of the candidate's hashing establishes
the expected ID for each operation. Exhaustive small fixtures and
deterministically generated traces exercise membership, duplicate
insertion, reservation, growth, clear and reuse. Compare after every
operation, not just after all keys have been inserted.

| Area | Required checks |
| --- | --- |
| Probing | Empty and one-entry stripes; minimum and larger capacities; every possible home position; wrapped walks; exact load boundary and one addition across it; misses following long clusters |
| Group termination | Matching lanes and emptiness in one group; full groups before a partial group; last-group wrap; every lane of a match mask; group-full collision overflow; no skipped match at an empty control |
| Collisions | Same home/different tags; same tag/different keys; identical full hashes for unequal tuples; hash zero; all tuples assigned to one stripe; collisions through multiple growths |
| Growth | No-op reserve, ordinary growth, multi-doubling batch reserve, prepare/abandon, prepare/complete; source lookup before completion; no allocation during completion; cached growth makes zero column-rehash calls |
| Identity | Arity 0, 1, 2 and 32; identical prefixes with last-column differences; symbol/string type differences; ordinary, hashed-batch and reserved append give the same IDs and iteration order |
| Ownership | Empty-payload adoption, populated-base append, refused load, failed derived evaluation, repeated queries, clear/reuse and destruction; exactly one release and charge per block |
| Bounds | Checked projections near `SIZE_MAX` and tuple-ID limits without huge allocation; initialized controls/IDs; final group loads in bounds; nonzero bit scans; portable and vector masks agree |
| Accounting | Actual versus projected bytes after success and each injected failure; limits immediately below, at and above growth thresholds; held prepared replacements; no charge left after abandoned work |
| Parallelism | One and multiple stripe writers separated by barriers; immutable frozen screens; forced deferral at one worker; 1/2/4/16 workers and supported stripe-count test builds agree |

Mechanism regressions measure meaningful avoided work. A controlled
miss-heavy fixture demonstrates fewer exact comparisons for a filtering
layout. A growth-heavy fixture demonstrates removal of column rehashing
for the cached-hash layout. Establish the asserted improvement against
the reference before implementing it; keep wall-clock thresholds out of
unit tests. Fixtures with identical fingerprints/full hashes must still
compare exact keys, so a shortcut accepting a hash collision fails.

Run the language/golden, random differential and relevant performance
counter suites through the ordinary library/CLI interface. The naive
strategy alone is insufficient: it shares relation storage. Compare
against independently checked answers and the immutable reference
binary as well. With ample limits, compare unsorted output, IDs where
available, explain, and semantic counters; account for layout-dependent
storage bytes. With limits, compare worker counts within each candidate
and validate its own predicted refusal prefix and retained values.

Before implementation tests and code commit, apply the repository's
review requirement, explicitly covering security, information leaks and
readability. Review the probe termination proof, equality boundary,
projection/allocation agreement and ownership transfer in the actual
functions. Run the full normal suite, ASan and UBSan for storage and
mask arithmetic, TSan for frozen readers and stripe writers, and LSan/
MSan where supported. Allocation failure is injected at each new owned
allocation and a later operation verifies the context remains usable.

### Workload and attribution matrix

Target complete `tc-cyclic`, `tc-acyclic`, `tc-sparse-cyclic`, `sg-cyclic`
and `sg-acyclic` runs. Include `tree-sg-all` as the large storage case and
`chain-closure` as the growth control. Measure both successful and
unsuccessful membership phases within these runs. A successful frozen
lookup still reads the matching tuple in every layout; tags only remove
other candidates, while cached hashes additionally remove growth reads.

Controls include sparse reachability, Andersen/CSPA analyses, `facts-join`
loading, `files-grouped`, `degree-count`, startup/small relations, and
synthetic high-arity relations. Query factoring can leave membership with
little work; such a case is a useful control for an unconditional storage
or construction tax. Separate uniformly distributed and stripe-skewed
inputs and synthetic collision traces. Collision fixtures establish
correctness and tail costs; they are not a substitute for production
hash distributions.

Choose a target and controls from these families under the
[measurement protocol](../docs/todo-performance-measurements.md).
Begin at one representative size and one/sixteen workers; add sizes or
worker counts only to resolve a specific storage or scaling question.
Keep binaries, data, compiler options, harness and output sink immutable.
Each decisive cell has at least ten interleaved AB/BA pairs with raw spread,
wall and summed user CPU, peak RSS, charged steady/peak storage and exact
answer evidence. Large runs that cannot finish remain failed/limited
statuses. Diagnostic counter, sampling and timed-production cohorts are
separate artifacts.

Compare the ordinary production link layout first. Small hot-loop changes
can alter surrounding code placement. A close result may justify one
predetermined layout control using the same transformation for reference
and candidate. Retain every result and close an unresolved comparison as
inconclusive. Keep compiler, LTO and alignment policy fixed across a
representation comparison; selecting a build policy is a separate decision.

Production runs retain ordinary random context seeds. Controlled seeds
belong only to synthetic diagnostic fixtures, which can isolate collision
behavior without changing the library's entropy contract. Compare each
workload/scale/thread cell separately; aggregate summaries cannot conceal
a repeatable regression or turn distinct cells into repeated samples of
one measurement.

A reproducible cohort records source/binary hashes, commands and flags,
workload data hashes, capacities and occupancy, diagnostic distributions,
raw timing samples, exact-answer checks and memory/refusal observations.
Use local artifacts under `bench/compare/recorded/` and link their report
from the work plan. No source instrumentation counter becomes public
API merely to support these measurements.

### Adoption rule

Selection follows four gates in order:

1. **The cost is still material.** The intended pipeline's profile and
   counters identify repeated canonical work that the candidate removes.
   If other work dominates, retain the reference and direct optimization
   at that work instead of widening every relation.
2. **The mechanism is correct and visible.** Differential/failure tests
   pass, and diagnostic counts demonstrate the claimed reduction in
   tuple comparisons or growth hashing. Sampling alone is insufficient.
3. **Complete workloads improve.** Targeted closure and same-generation
   runs show repeatable wall improvement with supporting CPU evidence,
   exceeding the observed timing and code-placement uncertainty. A
   microbenchmark win, one unusually large growth or one selected scale
   does not pass. A reproducible unexplained regression in a control
   blocks default adoption.
4. **Memory and complexity earn their cost.** Report absolute bytes,
   relative payload/canonical increases, growth peaks and the largest
   admitted datasets. Prefer the simplest representation when outcomes
   are indistinguishable. A wider representation needs a demonstrated
   gain unavailable from the cheaper candidates; a memory improvement
   without a useful complete-run benefit is not the locality goal.

The selection report states the complete-command gains, storage costs
and remaining gaps. A probe speedup does not establish an engine lead
over another implementation or commit the project to a larger pipeline.

Only the selected mechanism enters production. Update the canonical
index contract, the identifier table's owner choices and the memory
projectors together; remove experimental dispatch and rejected storage
paths. If every candidate fails a gate, retain the reference and record
the measured reason and artifact in the alternatives below. No finding
is inferred before its experiment, and no failure to select a new layout
requires carrying its maintenance overhead in the engine.

## Alternatives not taken

* **Implementing every candidate before establishing its bottleneck.**
  The [retained-engine study](../bench/compare/recorded/m3-max/membership-growth/README.md)
  demonstrates useful comparison filtering with the existing linear tags.
  Its sampled growth cost includes scanning and placement that a cached
  hash would retain; the hash callback alone does not justify a wider
  table on those workloads. Grouped controls and cached hashes need
  separate evidence of a benefit beyond the existing tags. High-arity growth
  is outside that evidence, so the conclusion is limited to the measured
  workloads rather than a universal claim about either representation.
* **A runtime layout chooser or per-relation adaptation.** Lookup timing,
  observed collisions and worker count would complicate deterministic
  capacity and refusal. One selected representation needs no migration,
  hysteresis, statistic catalog or estimate on every insertion.
* **Widening every generic identifier table.** Canonical lookup and
  growth have a different cost balance from symbol bytes, small rewrite
  tables and secondary keys. A canonical result does not justify that tax
  for unrelated owners.
* **Caching tuple keys in slots.** Copying all columns makes slot width
  depend on arity and duplicates the payload needed for scans and joins.
  An arity-specific inline key also adds representation cases at adoption
  and growth. The full-hash candidate isolates growth/filtering at a
  fixed cost first. Key duplication needs distinct evidence of a
  remaining unavoidable successful-lookup bottleneck.
* **A separate dense tuple-hash cache.** It can be smaller than a hash
  per table slot, but adds a payload-sized allocation, its own growth and
  a dependent ID-to-hash read. The slot cache tests the locality benefit
  directly. If its memory cost prevents adoption while growth remains
  material, a dense cache is a separate measured design decision, not
  another hidden mode in this table.
* **Combining grouped controls, full hashes and cached keys immediately.**
  Their maintenance and memory costs accumulate while attribution is
  lost. Add no combination without evidence that the selected simple
  candidate leaves a separate material cost.
* **Changing tuple order to match hash order.** Delta ranges, secondary
  links and unsorted output depend on stable insertion-order IDs. The
  membership layout has no authority to reorder the tuple store.
* **Fingerprints as equality, unkeyed slot placement or collision-driven
  resizing.** The first loses answers, the second weakens the input
  flooding defense, and the third makes memory admission depend on the
  secret seed. Exact equality, keyed placement and count-based capacity
  are common constraints of every experiment.
* **Robin Hood relocation, deletion metadata or a cache-line-specific
  allocator.** Append-only membership needs none of these for
  correctness. Each requires evidence beyond a grouped-control lookup
  result before adding another invariant or portability mechanism.
