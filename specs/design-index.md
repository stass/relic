# relic index design

**Status.** Design document for secondary indexes: what an index over some of
a relation's columns holds, how a lookup walks it, how it is kept in step with
insertion, when it is built, and what it costs against the memory limit.
Forward-looking, like every document under `specs/`; `STATUS.md` records what
is implemented.

**Companions.** `specs/architecture.md` sections 20, 22, 58 and 63;
`specs/design-relations.md` for the store an index points into and the
canonical full-tuple index beside it; `specs/design-planner.md` for which
indexes a plan asks for; `specs/design-plan.md` for the `LOOKUP` operation
that reads one; `relic-plan.md` decisions D-3, D-4, D-6, D-7 and D-10.

---

## 1. What this document settles

* what a secondary index is keyed on, and what it stores for a key;
* why a key's tuples form a chain rather than sharing a probe cluster;
* what a lookup visits, in what order, and what it does not see;
* how an insertion extends every index without a failure leaving one behind;
* when an index is built, how building over tuples already held can fail, and
  why a half-built index is never published;
* how long an index lives and how a plan refers to one;
* what an index costs against `max_memory`.

It does not settle which indexes exist. That is the planner's question, and
`specs/design-planner.md` answers it: an index is built for exactly the bound
columns of some literal a plan looks up.

---

## 2. What an index is

A relation of arity *n* has one canonical index over the whole tuple, which
answers membership, and any number of **secondary indexes**, each over a
nonempty proper subset of its columns. The subset is the **key mask**, a
`uint32_t` with bit *c* set for column *c*; D-7 caps arity at 32 so that one
word holds every subset.

A secondary index answers one question: *which tuples hold these values in
the key columns?* `depends(parser, X)` asks it of `depends/2` with key mask
`{0}` and key `(parser)`, and the answer is every tuple whose column 0 is
`parser`.

The **key** of a tuple under a mask is its key columns' values in ascending
column order. Two tuples share a key exactly when they agree on every key
column. A relation holds at most one index per mask.

---

## 3. Representation

```c
struct relic_index {
	relic_idtab_t		 keys;		/* key hash -> key identifier */
	relic_index_key_t	*entries;	/* per key: its first and last tuple */
	relic_tuple_id_t	*next;		/* per tuple: the next with its key */
	size_t			 key_count;
	size_t			 entry_capacity;
	size_t			 next_capacity;
	uint32_t		 key_mask;
};

struct relic_index_key {
	relic_tuple_id_t	first;
	relic_tuple_id_t	last;
};
```

Each distinct key gets a **key identifier**, assigned from one in the order
keys are first seen. `keys` is the open-addressing identifier table of
`specs/design-memory.md`, mapping the hash of a key to the identifiers that
share it; `entries[k - 1]` records the first and the last tuple holding key
*k*. `next[id - 1]` is the tuple after `id` holding the same key, or
`RELIC_ID_NONE` when `id` is the last. A key's tuples are therefore a singly
linked **chain** threaded through `next`, from `first` to `last`.

Nothing is stored twice. A key is not copied into the index: comparing a
probe's key against key *k* reads the key columns of `entries[k - 1].first`,
which is in the relation already. A tuple is not copied either; the index
holds identifiers.

### 3.1 Why chains

The identifier table already stores identifiers inline, and the obvious
alternative is to put every tuple identifier into it directly, several under
one key. Under linear probing that is quadratic: the tuples sharing a key
occupy consecutive slots, so a key held by a thousand tuples is a probe
cluster of a thousand, and every other key whose hash lands in or before it
walks the whole cluster to find its own. The skew is the normal case in
Datalog -- one parser depended on by every module -- not an adversarial one.

With chains the table holds one slot per distinct key whatever its tuple
count, so a probe's length depends on the number of keys and not on how the
tuples fall among them, and walking a key's tuples is following `next` rather
than probing.

### 3.2 Why insertion order

A chain is extended at `last`, so it lists its tuples in insertion order,
which is identifier order. D-4 requires that relations iterate in insertion
order, and a lookup that walked a key's tuples in hash order would make the
order of derived tuples depend on a growth history. With chains in
identifier order, a `LOOKUP` visits exactly the tuples a `SCAN` would have
matched on the key columns, in the order the scan would have visited them.

---

## 4. A lookup

```c
relic_tuple_id_t relic_index_first(const relic_index_t *,
    const relic_relation_t *, const relic_dom_t *probe);
relic_tuple_id_t relic_index_next(const relic_index_t *, relic_tuple_id_t);
```

The probe is laid out as a tuple of the relation, with the key's values in
their own columns; the other columns are not read. `relic_index_first()`
gathers the key from it, hashes the key, probes `keys` comparing each
candidate key's columns against it, and returns the first tuple of the
matching chain, or `RELIC_ID_NONE` when no tuple holds the key.
`relic_index_next()` follows the chain.

How a key is packed -- which columns, in what order -- is known to the index
alone. Insertion gathers a key from a stored tuple and a lookup from a probe
by the same function, so the two cannot disagree about the layout, which
would make every lookup miss without any error.

**A lookup sees the relation as it stood when the lookup started.** The
executor takes the relation's end when a `LOOKUP` starts and stops at the
first chain member at or beyond it. That is exact because identifiers
increase along a chain: once one member is past the end, every later one is
too. It is the same visibility a `SCAN` has, and for the same two reasons:

* an `EMIT` below the lookup may insert into the very relation it walks,
  extending the chain it is on. Without the bound the lookup would visit what
  its own join derived, which is correct but makes a rule's work depend on
  the order of its insertions, and in a self-recursive plan it need not
  finish in one pass;
* the semi-naive completeness argument of `specs/design-eval.md` section 4.2
  is written in terms of what a full relation held when an operation started,
  and a lookup is a full-relation read.

A lookup holds no pointer into the relation or the index across the
operations below it, only the next identifier and the end. An insertion below
may grow `next`, `entries`, `keys` and the column array, and none of that
invalidates two integers.

The key columns of a candidate need not be compared again. Every tuple on a
chain holds the key, because a chain is found by comparing key columns, not
hashes. The executor compares the other columns as a `SCAN` would; it may
compare the key columns too, which is redundant and harmless.

---

## 5. Insertion keeps every index current

An index is extended as tuples are inserted, never rebuilt. A fixed point
inserts into a relation round after round, and an index invalidated by each
insertion would be rebuilt every round.

The relation's insertion order of `specs/design-relations.md` section 6 gains
one step, and keeps its shape -- every fallible step before every mutation:

1. look the tuple up in the canonical index; a duplicate adds nothing;
2. check the limits against what the relation *and every index* will hold;
3. reserve room in the canonical index and for the columns;
4. **reserve room in every secondary index for one more tuple and one more
   key**;
5. append the columns, place the canonical entry, and **add the tuple to each
   secondary index**. Nothing here can fail.

**Room for one more key is reserved whether or not the key is new.** Whether
it is new is a probe away, and the probe would have to be made twice, once to
decide what to reserve and once to add. Reserving unconditionally means an
index may grow one step sooner than it had to -- once, and then it has room
for many keys. What matters more is that the projection of step 2 and the
reservation of step 4 make the same assumption, so that the memory limit is
charged for exactly what is allocated, as the canonical index already is.

Adding a tuple to an index is then: compute its key's hash; probe for the key;
if it is there, link the tuple after the key's `last` and make it the new
`last`; otherwise assign the next key identifier, record the tuple as both
`first` and `last`, and place the identifier. In both cases the tuple's own
`next` is `RELIC_ID_NONE`.

A tuple stored but missing from a secondary index would be a lookup that
misses a tuple the relation holds: a join that silently drops answers, with no
error. Step 5 cannot fail, and it comes after the last step that can, so that
state is unreachable rather than avoided, exactly as for the canonical index.

---

## 6. Building an index

The planner asks a relation for an index by mask. If one exists it is
returned; otherwise one is **built**, over every tuple the relation already
holds, before the planner returns.

```c
int relic_relation_index(relic_t *, relic_relation_t *, uint32_t key_mask,
    uint32_t *positionp);
```

Building is insertion of each existing tuple, in identifier order, into an
empty index: check the memory limit against what the index will hold, reserve,
add. Doing it in identifier order is what makes every chain of a built index
identical to the chain insertion would have produced.

**A half-built index is never published.** The index is appended to the
relation's list only once every tuple is in it. A build that runs out of
memory or reaches the memory limit part way releases what it built and
leaves the relation with the indexes it had. An index holding some of a
relation's tuples would be indistinguishable from one holding all of them,
and every lookup through it would miss the rest without a word.

While a build is in progress its storage is counted in the context's total,
so the limit sees it; a failed build takes its bytes back out as it releases
them. The limit diagnostic says what was being done:

```text
relic: limit exceeded: memory=1048576 while indexing edge/2
```

Most indexes are built empty. A component is planned when it starts, and it
creates indexes over its own members, which are empty then, and over relations
of earlier components, which are complete; a query is planned over relations
that are all complete. So an index over a relation that is still growing is
always built before the growth starts, and an index built over tuples already
held is built once, over a relation that will not change again in that
evaluation.

---

## 7. Lifetime, and how a plan names an index

**An index lives until its relation is next discarded.** Once built for a
mask, it is kept current by every insertion into its relation, through the
rest of the evaluation that asked for it and through any query or later
evaluation that finds it already there. Discarding every relation, which is
what a change to the program does, drops every index along with the tuples.

Keeping an emptied index would save nothing. The next evaluation's planner
asks for the masks its plans need when each component starts, and an index
built then, over whatever tuples its relation holds by that point, costs the
same per tuple as keeping it up through those insertions would have: one
chain link and at most one key each (section 6). What dropping buys is that the indexes a relation keeps up, and pays for on every insertion
and against the memory limit, are the ones asked for since the program last
changed. A query or rule of an earlier program that looked a relation up on
another binding pattern does not tax every later evaluation, and whether an
evaluation fits a limit does not depend on what an earlier one asked
(`relic-plan.md` decision D-4).

A relation's indexes are held in a growable array, so an index's address
moves when another is built. A plan does not hold the address. A `LOOKUP`
names its index by **position** in its relation's array, which is fixed from
the build until the next discard because indexes are only appended between
discards, and the executor reaches it through the relation, which never
moves. A plan can then be built in any order -- one rule's plan creating an
index after another rule's plan chose one -- with nothing for either to know.

A discard happens only before an evaluation begins, and a plan lives only
within one, so no plan names an index a discard dropped.

---

## 8. Accounting

An index's `next`, `entries` and `keys` arrays are relation and index storage
in the sense of architecture section 63, and count toward `max_memory` by the
same method the relation store uses for its own arrays: the growth policies
are asked what they will allocate before anything is allocated, and the total
is reconciled from what the arrays hold before and after a reservation, so no
failure path has to give anything back by hand.

The array of index headers does not count, for the reason the array of
relation headers does not: it is sized by the program -- by the binding
patterns its rules look relations up with -- and not by the data.

Per tuple, an index costs four bytes of `next`, plus at most eight bytes of
`entries` and some fraction of a four-byte slot per distinct key. A relation
looked up under three masks therefore pays roughly as much again as its
columns, which is the price of not scanning.

---

## 9. Determinism

* A key's hash folds its columns as words in ascending column order, with
  the context's seed of `specs/design-hash-seed.md`, which decides nothing
  a lookup reports.
* Key identifiers are assigned in first-seen order and chains are in
  identifier order, so the order a lookup visits tuples depends only on the
  order they were inserted.
* An index is found by mask, not by address, and a plan refers to one by
  position.

---

## 10. What is deliberately absent

* **Removal.** Indexes follow the relation store: nothing removes one tuple,
  so `keys` needs no tombstones and a chain is never unlinked.
* **Indexes over a delta.** A delta literal is pinned first in its variant and
  always scanned, per D-10, so no plan looks a delta up.
* **Range and prefix lookups.** Every comparison is a filter and nothing in
  the language asks for an ordered traversal.
* **Reusing an index over fewer columns.** A plan that binds columns `{0,1}`
  gets an index on `{0,1}` even when one on `{0}` exists.
  `specs/design-planner.md` section 5 explains why.
* **Rebuilding on insertion.** Section 5.
