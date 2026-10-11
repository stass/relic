# Storage

Where a predicate's tuples live, how a tuple is found, and what makes the
store safe to evaluate over. A relation is a set of tuples of one arity, held
as a flat array of domain identifiers with a hash index over the whole tuple;
secondary indexes over some of its columns serve the lookups plans make. The
store is a foundation: it sits between interning ([values](design-values.md)),
which turns values into the identifiers it holds, and the evaluator
([evaluation](design-eval.md)), which reads relations through plans and
inserts what rules derive. Three properties carry everything above it: tuple
identifiers are assigned in insertion order and never change within an
evaluation, so a delta is a range of identifiers; an insertion reports
newness separately from failure; and a failed insertion leaves nothing
behind.

Related: [architecture: storage](architecture.md#storage),
[architecture: limits](architecture.md#limits), [memory](design-memory.md),
[values](design-values.md), [facts](design-facts.md),
[planning](design-planning.md), [evaluation](design-eval.md),
[parallel evaluation](design-parallel.md).

## Model

**A predicate has at most two relations.** Its **base store** holds the facts
the program states for it; its **derived relation** exists once a rule derives
it and holds what evaluation puts there. Each lives in a table of its own,
indexed by predicate identifier and extended the first time a relation for a
predicate is asked for. Two functions hand relations out, one for the relation
a rule writes and one for the relation an operation reads, and below them
nothing asks which kind a relation is; why only a commit writes a base store,
and why that makes discarding safe, is in
[the read and write funnel](design-facts.md#the-read-and-write-funnel).

**One relation lives outside the tables: a query's answer.** It belongs to no
predicate, is owned by the query object, is emptied at both ends of every run
and is released with the query. To the store it is a relation like any
other, counted against the same limits; only its ownership differs. A query's
group relations, which hold the groups its aggregates fold, are ordinary
derived relations filled by the query's **group run** -- the step that runs
each of the query's group rules once, in an arena of its own
([aggregates](design-aggregates.md#queries)) -- and emptied when it
finishes.

**The tables hold pointers to relations, not relations.** A relation is
created once and handed out repeatedly: a plan resolves the relation of each
body literal, an evaluation holds the head's relation across a rule, and both
outlive the creation of other relations. Stored inline, relations would move
whenever a table grew, and every holder would have to know not to keep a
pointer across a call that might create one -- a rule obeyed at a growing list
of call sites. Each relation is its own allocation, so growth moves pointers
and never relations, a relation's address is fixed for as long as it exists,
and there is no rule for a caller to obey. For the same reason a relation
names its predicate by identifier, not by a pointer into the growable
predicate table.

Relations are database state, not program structure, which is why they live in
tables of their own rather than in the predicate's record: the derived
database can be discarded without touching the program that derived it.

## Invariants

* **A relation is a set.** Every tuple it holds is distinct, and inserting one
  it holds changes nothing and says so.
* **Tuple identifiers are assigned from one, in insertion order**, and zero
  names no tuple. A relation holds at most 2^32 - 2 tuples; the ceiling is
  checked, since an identifier that wrapped would silently turn one tuple into
  another.
* **Append-only within a span.** Between two clears of a relation -- and a
  discard clears every derived relation -- nothing compacts, deletes,
  reorders or renumbers its tuples.
* **Stored implies indexed.** Every tuple a relation holds is in its canonical
  index and in every secondary index it has.
* **Chains are in identifier order**, so a lookup visits tuples in the order a
  scan would.
* **The totals are sums.** The tuple total and the storage total are at every
  moment the sum of what every relation holds, in a table or not.

## The tuple store

Tuples are stored end to end in one array of domain identifiers with the
arity as stride: the tuple with identifier `id` occupies columns
`(id - 1) * arity` up to `id * arity`. A tuple has no header, length or
pointer of its own and costs four bytes per column whatever its values are.
The store never inspects a value: a column is an opaque 32-bit integer the
caller has already interned, so membership, hashing and duplicate elimination
are integer work.

Capacity is counted in columns rather than tuples so that one growth policy
serves every arity, the empty one included. The column count `count * arity`
is checked before it is formed: a 32-bit identifier space and an arity of at
most 32 fit a 64-bit `size_t` but not a 32-bit one.

**The empty tuple takes the general path.** The key of an arity-zero tuple is
the empty sequence; its hash is defined, two empty keys compare equal, and its
stride is zero, so the second insertion finds the first and adds nothing. The
only visible consequence is a minimum-size column array that holds nothing,
which costs a few dozen bytes and buys the absence of a special case.

## The append-only invariant

The store's interface offers insertion, a load's reservation and append into
base stores, a round's append, membership, reading a tuple by identifier, and
exactly two operations that remove anything, both of which remove relations
or indexes whole:

* **clearing one relation**, which a query's answer undergoes at both ends of
  a run and a query's group relations (the relations its aggregates fold)
  undergo when the query's group run finishes, and which drops the
  relation's indexes with its tuples;
* **discarding everything derived**, which frees every derived relation with
  its indexes and drops the base stores' secondary indexes, and never removes
  a base store's tuples. When it runs, and the mark that makes a discard with
  nothing to discard free, are in [the discard](design-facts.md#the-discard).

So the invariant is a property of the interface, not a rule callers observe.
A clear ends one **span** of a relation and begins another, and a discard
ends the span of every derived relation: identifiers restart from one, so a
range taken before names nothing, or other tuples, afterwards, and the
relation's indexes, with their positions and frontiers, are gone. The
obligation a clear takes on is that no range, index position or (after a
discard) derived relation pointer outlives its span, and the evaluator meets
it by construction: every range and plan lives in the state of the
evaluation or group run that uses it, a discard runs only outside an
evaluation, and a query's answer and group relations are cleared only before
or after the run that reads them. A write through a pointer kept across a
discard is a use after free, which the sanitizers report, rather than tuples
that survive the discard unnoticed. Each derived relation is cleared before
it is freed and each index gives back what it counted, so the totals stay
sums; afterwards they count the base stores alone, which an assertion
confirms.

**Semi-naive evaluation rests on this.** The tuples derived in one round are a
half-open range of identifiers rather than a separate relation, so a delta
needs no copy and no second store. A range is a value -- two identifiers, no
pointer into the relation -- so a growth during iteration cannot invalidate
it. Any future incremental insertion or deletion support must confront this
first: the moment something can renumber a relation, a range captured before
it means something else afterwards.

**Iteration order is insertion order**, because identifiers are assigned in
insertion order. A relation iterated in hash order would make output depend on
a hash seed and a growth history.

## The canonical index

Each relation has one index whose key is the whole tuple and whose value is a
tuple identifier. It answers membership, which duplicate elimination,
membership tests in plans and fixed-point detection all reduce to. It is the
identifier table of [values](design-values.md#the-identifier-table):
a slot holds a tuple identifier and nothing else, the key stays in the column
array, and no tuple is stored twice. It is untagged, because its searches are
mostly successful.

**It is striped.** It is a constant number of identifier tables, and a tuple's
entry lives in the one its wide hash's unkeyed top bits choose. A stripe is
found from the hash alone, so a lookup probes one table as an unstriped index
would, and tuples of different stripes can be placed at once, which is what
lets a round's merge place its tuples in parallel.

**Hashing is over words, not bytes.** A tuple is hashed by folding its columns
as 32-bit words. A byte hash of a word array probes in a different order on a
big-endian machine than on a little-endian one.

**Equality is a column loop, not a byte comparison.** For a dense word array a
`bcmp()` would be equivalent, except at arity zero: `bcmp(NULL, NULL, 0)` is a
comparison at a null pointer, which the standard does not permit and a
sanitizer reports. The loop is defined for every arity and needs no case for
the one that would break the other form.

**A load's hash is carried in.** Every relation and staging set of a context
hashes a tuple with the same function under the context's seed, so the hash a
load's staging computed for a fact is the one its base store would compute;
membership and the base append take it as a parameter, and a fact is hashed
once on its way in. A debug build recomputes and compares.

## Insertion

**Newness is an output, separate from failure.** An insertion reports through
an out-parameter whether the tuple was not already present, and returns
failure separately. That single bit is what fixed-point detection is built on:
a round that added no tuple to any relation has reached the fixed point, with
no comparing, counting or sorting. A two-valued return would conflate a
duplicate with a failure, and they are opposite answers: a duplicate means the
fixed point is closer, a failure means the answer is unknown.

**Every fallible step comes before every mutation:**

1. **Look the tuple up** in the canonical index. A tuple already present is
   reported as not added, and nothing is allocated.
2. **Classify its key in every secondary index**: hash it, probe its stripe,
   and note the key it joins or that it is new. Nothing is changed.
3. **Check the limits** against what the relation and every index would hold
   with the tuple in: the tuple limit, and the memory limit by the
   projection of [memory](design-memory.md#the-memory-limit).
4. **Reserve room** in the tuple's stripe of the canonical index, in the
   columns, and in every secondary index -- for a key only where the key is
   new. These are the steps that can fail.
5. **Append the columns, place the identifier, and link the tuple into every
   secondary index** as step 2 classified it. None of these can fail.

A tuple stored but not indexed is not a lost tuple; it is a tuple membership
cannot find, so the next insertion of the same columns appends a second copy,
and a relation holding a duplicate is not a set -- every count, join and
fixed-point decision downstream is wrong and nothing reports an error.
Likewise a tuple missing from a secondary index is a join that silently drops
answers. Because storing and indexing both come after the last step that can
fail, and neither can fail itself, those states are unreachable rather than
avoided. A failed insertion leaves contents and indexes agreeing and holding
what they held; capacity a partial reservation obtained is kept, and
accounted, and the next insertion uses it.

**The key is probed for once.** Step 2's note is what step 5 links by. The
reservation in between may move a stripe's arrays and rehash its table, but it
changes neither which keys the index holds nor their identifiers, and the
note records an identifier and a hash, never a position, so it remains true
when step 5 reads it. Knowing before reserving whether a key is new is what
lets the projection and the reservation account for exactly the growth the
insertion causes.

**Batch insertion** inserts many tuples exactly as that many single insertions
would, in order -- the same tuples added under the same identifiers, refused
at the same one with the same diagnostic -- and exists so that the cache
misses of the duplicate checks overlap rather than follow one another. Single
and batch insertion both hash first and then perform one insertion by that
hash, so they cannot disagree about what a duplicate is or where a limit
falls.

**A load's commit reserves, then appends.** The commit has already looked
every staged fact up and knows how many each base store gains. The store
checks both limits once, against the totals plus everything the commit adds,
and refuses the whole load before any store is created or grown. It then
reserves room in each store, growing its columns and each stripe of its
canonical index at most once to exactly the bytes projected, and the commit
appends each fact into that room without looking anything up; no append can
fail. Such a reservation is made only of a relation without secondary
indexes, which a base store is at commit because the commit discards first:
what a secondary index would grow by depends on how many facts bring a new
key, which the commit has not asked, so a reservation of a relation with one
is refused as an internal error rather than made short.

**A round's merge appends many new tuples at once.** A **deferred round**, one
whose insertions wait for its end
([parallel evaluation](design-parallel.md#model)), first *screens*
its candidates against the relation read-only -- the lookup half of an
insertion, modifying nothing -- so a tuple the screen finds is exactly one an
insertion would have found. The merge then gives the relation all its new
tuples, in the identifier order the serial round would have given them: their
keys are classified in every secondary index, the bytes the relation would hold
are projected exactly (columns and indexes at the stepwise capacity one
insertion at a time would reach), the limit is asked once, room is reserved,
and then each stripe of the canonical index and of every key table is filled
by a part of its own, on its own thread. Parts never write a word another part
writes: the slot of a chain's last tuple in the shared `next` array belongs to
that chain, and a new tuple's slot to the new tuple. A merge the projection
admits cannot be refused by the memory limit; one it refuses is run again the
serial way.

## Secondary indexes

A secondary index answers one question: *which tuples hold these values in
these columns?* `depends(parser, X)` asks it of `depends/2` with key columns
`{0}` and key `(parser)`. The columns are a **key mask**, one bit per column,
which the arity limit of 32 lets one word hold. The **key** of a tuple under a
mask is its key columns' values in ascending column order. A relation holds at
most one index per mask, and only indexes some plan asked for: an index is
built for exactly the bound columns of a literal a plan looks up
([planning](design-planning.md)).

**Representation.** Each distinct key lives in one stripe, chosen by its wide
hash, and has a key identifier within the stripe, assigned in first-seen
order. Per stripe: an identifier table from key hash to key identifier
(tagged), and per key its first and last tuple, its frontier, and its values.
Per index: one `next` array by tuple, giving the next tuple with the same key.
A key's tuples are therefore a singly linked **chain** through `next`, from
first to last. `lib/relation.h` has the layout.

**Why chains.** The obvious alternative puts every tuple identifier into the
table directly, several under one key. Under linear probing that is
quadratic: a key held by a thousand tuples is a cluster of a thousand slots,
and every other key hashing into or before it walks the cluster. Skew is the
normal case in Datalog -- one parser depended on by every module -- not an
adversarial one. With chains the table holds one slot per distinct key, so a
probe's length depends on the number of keys, not on how tuples fall among
them, and walking a key's tuples is following `next`.

**Why insertion order.** A tuple is linked at its chain's end, so chains list
tuples in identifier order. A lookup therefore visits exactly the tuples a scan
would have matched on the key columns, in the order the scan would have
visited them, and the order of derived tuples never depends on a growth
history.

**A key's values live in the index.** Nothing would be lost by reading the key
columns of the key's first tuple instead, but a probe compares against the key
of every slot it visits whose tag matches, and the first tuple is a random
access into the relation for each; the index's own key values, one key's after
another's, share cache lines with their neighbours. A key costs its columns
again, one to a few words. Gathering a tuple's key walks the listed key
columns rather than every bit of the mask, and the key table's rehash reads
the key values in order.

### Lookup

A lookup is given a probe laid out as a tuple of the relation, with the key's
values in their own columns; the other columns are not read. It gathers the
key, hashes it, probes the stripe the hash chooses, comparing candidate keys'
stored values, and follows the matching chain. How a key is packed is known to
the index alone: insertion gathers a key from a stored tuple and a lookup from
a probe by the same function, so the two cannot disagree about the layout,
which would make every lookup miss without an error. Every chain tuple holds
the key, because a chain is found by comparing key values, not hashes, so the
executor need only compare the other columns.

**A lookup sees the relation as it stood when its plan started.** The executor
resolves each view when the plan starts and stops a walk at the first chain
member at or past the view's end; identifiers increase along a chain, so every
subsequent member is past it too. This is the visibility a scan has, for the
same two reasons: an emit below the lookup may insert into the very relation
being walked, extending the chain it is on, and without the bound the work of a
rule would depend on its insertion order and a self-recursive plan need not
finish; and the semi-naive argument that every derivation is made exactly once
rests on no operation of a round seeing what the round inserted
([evaluation](design-eval.md)).

A lookup holds no pointer into the relation or the index across the
operations below it, only the next identifier and the end, so an insertion
below that grows `next`, the stripes or the column array invalidates nothing.

### Lookup of a delta

A lookup through a delta view visits the key's tuples in the delta
`[since, end)`. Chains are in identifier order, so those tuples are a
contiguous run of the chain. Stopping at `end` is what every lookup does;
*starting* at `since` without walking the chain from its head is what the
**frontier** is for, and it is what lets the planner place the delta literal
anywhere in a body instead of first. A check of a delta -- an existence check
keyed on some columns -- asks for the run's first tuple alone and finds it the
same way.

Each index records the **recorded start**, the largest delta start it has been
raised to; the evaluator raises every index of a component member to the start
of the member's delta wherever it sets the delta, which is the one place a
delta is set, so every lookup and check of one round asks the same `since`. A
key's frontier is a tuple of its chain with one checkable invariant: every
chain tuple before it is below the recorded start. Delta starts never decrease,
so the recorded start is at or below every start the index is asked for from
then on, and the invariant is what the walk needs. Raising only loosens the
bound and touches no frontier; moving a frontier only on a walk to the recorded
start itself, and never past a tuple at or above it, keeps the invariant. A new
key's frontier is its first tuple, with nothing before it.

A delta lookup walks forward from the key's frontier while the identifier is
below `since` and returns the tuple it reached. When `since` is the start the
index was raised to, the frontier moves to that tuple, or, when the walk ran
off the chain's end, to the chain's last tuple, which lies below `since`.
A frontier is never empty, so tuples linked after `last` in a subsequent round
stay reachable from it. A frontier step reads `next` and no tuple, and is not
counted as a tuple looked up.

**It is amortized constant time because deltas advance.** Within one fixed
point a member's next delta starts where the previous one ended, so the
`since` an index is asked for never decreases, a frontier only moves forward,
and it passes each chain tuple at most once over the whole fixed point. After
the first lookup of a key in a round, every subsequent one of that round walks
nothing. Positioning costs at most one pass over the relation however many
rounds and lookups there are.

**It is correct whatever it is asked.** The invariant depends on `since` never
decreasing, which the evaluator guarantees: a clear or discard drops the
indexes, and a relation is a member of one component. An index built at a
later round boundary is initialized to the member's current delta start
before any prepared plan can use it. The guarantee is checked rather than
trusted. A call whose `since` is below the recorded start walks the chain
from its head and leaves the frontier where it
is; it costs the chain's length and happens on no path the evaluator has. A
call whose `since` is past the recorded start is a caller that did not raise
the index, which a debug build stops at: it could move nothing, since the
tuples it passes need not lie below every start to come, and every such lookup
would silently walk the same stretch again.

**Several lookups at one `since` may walk one chain at once** on the
workers of a shared round. The frontiers are an array of their own, reached
through a pointer, so a lookup handed the index read-only can move them, and
each is a relaxed atomic word; every walk of one round stops at the same
tuple, so the race is benign ([parallel evaluation](design-parallel.md) gives
the argument). The recorded start is written between rounds only.

### Building and lifetime

Preparing an executable plan asks a relation for an index by mask. An
existing one is returned; otherwise one is **built** over every tuple the
relation holds before preparation returns. A retained alternative can
record an access requirement without publishing an index. Building inserts
each existing tuple, in identifier order, into an empty index -- classify,
check the memory limit, reserve, link -- so
every chain of a built index is the chain insertion would have produced.

**A half-built index is never published.** The index joins the relation's list
only once every tuple is in it. A build that runs out of memory or reaches the
limit part way releases what it built and leaves the relation with the indexes
it had: an index holding some of the tuples would be indistinguishable from
one holding all of them, and every lookup through it would miss the rest
without a word. While a build runs its storage is in the total, so the limit
sees it.

Ordinary and delta-first plans acquire their indexes when the component
starts; a query acquires its indexes over complete relations. An index
needed only by a recursive alternative is acquired at the round boundary
where that plan is first selected. Such a build can read a populated,
growing relation. Every build runs at a preparation boundary with no
executor or worker active, using the same full-index construction and
failure contract.

**An index is extended by insertion, never rebuilt**, and lives until its
relation is cleared or the next discard. After a discard, preparation asks
for the masks the new evaluation's plans need. Rebuilding a derived
relation's demanded indexes costs the same per tuple as maintaining them
through that growth. What dropping buys is that the indexes a relation
pays for on every insertion and against the memory limit
are the ones acquired since the program last changed, so whether an
evaluation fits a limit does not depend on what an earlier program's queries
asked.

**A prepared plan names an index by position, not address.** A relation's
indexes are a growable array, so an index's address moves when another is built. A plan
names its index by position in the relation's array, fixed from the build to
the next discard because indexes are only appended, and reaches it through the
relation, which never moves. Plans can then be built in any order -- one rule's
plan creating an index after another's chose one -- with nothing for either to
know; and since a discard happens only outside an evaluation and a plan lives
only within one, no plan names a dropped index.

### Key counts for plan selection

A secondary index exposes its distinct-key count through a const accessor:
the sum of its existing per-stripe key counts. The accessor reads only
stripe headers, moves no frontier and never scans tuples or table slots.
Its result describes the entire owning relation, not a delta or old view.

The [recursive plan selector](design-recursive-planning.md#statistics-at-a-round-boundary)
combines this count with the tuple count of an operation's own view to
estimate lookup work. It reads the sum directly, with no evaluation-owned
statistics catalog or additional storage counter. An alternative's absent
index uses the selector's explicit provisional estimate and acquisition
charge; the store does not create an index to supply its statistics.

## Failure paths and limits

The store contributes two totals to the limits ([architecture:
limits](architecture.md#limits)): **tuples** across every relation, base stores
and query answers included, and the **bytes** of every relation's columns,
canonical index and secondary indexes, an index being built included. Bytes are
allocated capacity, not what a count implies. The tables of relation pointers,
the individually allocated relation headers and each relation's array of
secondary-index headers are not counted: they are sized by the program's
predicates and binding patterns, not by the data.

The memory account's rules apply as they do everywhere
([memory](design-memory.md#the-memory-limit)): every growth is
projected with the growth functions themselves before anything is modified,
and after a reservation the store's total is reconciled from what the
relation holds, so a reservation that grew both arrays, grew one and failed,
or grew nothing leaves the total exact with no failure path giving bytes back
by hand.

**When a limit fires the relation is exactly as it was**, because the check
precedes every mutation; consistency after a refusal is a property of the
ordering, not of an unwind path. The diagnostic names the limit, its value,
the activity and the relation, through one reporting function:

```text
relic: error: limit exceeded: tuples=1000000 while deriving path/2
relic: error: limit exceeded: memory=536870912 while indexing edge/2
relic: error: limit exceeded: tuples=4 while loading e/1
```

A load names the first predicate, in the order its facts name them, whose base
store it would grow. Discarding partial results is the evaluator's part; the
store's part is to refuse and stay consistent.

**Distinct-value counts** for the planner are computed when asked and cached
against the tuple count: a relation is append-only until cleared, so one
holding as many tuples as when a count was taken holds the same tuples, and no
insertion marks anything stale. Clearing forgets the counts.

## Determinism

* Tuple identifiers and key identifiers are assigned in insertion and
  first-seen order, chains are in identifier order, and iteration is by
  identifier, so no result is ordered by a hash.
* Tuple and key hashes fold words under the context's seed, which decides
  where an entry sits and nothing a relation reports.
* Stripes are chosen by an unkeyed hash and their number is constant, so
  capacities, the storage total and every limit decision are the same in every
  run and at every thread count.
* Nothing is keyed on an address: relations by predicate, indexes by mask and
  position. A relation's contents do not depend on how often its arrays grew.

## Alternatives not taken

* **Removing a tuple.** The only removals are clearing a relation and the
  discard of everything derived, which is what keeps the append-only
  invariant a property and lets the identifier tables go without tombstones.
* **A sorted or tree-shaped store.** Membership is the operation that matters
  and a hash answers it in constant time. Sorting is an output concern, done on
  the rows about to be printed.
* **Tuple identifiers directly in a secondary index's table.** Quadratic under
  skew; see [why chains](#secondary-indexes).
* **Range and prefix lookups.** Every comparison is a filter; nothing in the
  language asks for an ordered traversal.
* **Reusing an index over fewer columns.** A plan binding columns `{0,1}` gets
  an index on `{0,1}` even when one on `{0}` exists
  ([planning](design-planning.md)).
* **A separate relation per delta.** A range of identifiers is a delta with no
  copy, and the frontier gives it indexed access without an index of its own.
* **Statistics maintained on insertion.** Counts are computed on demand and
  versioned by the tuple count.
* **A per-relation memory limit.** The limit that protects a user is the
  total; a program that explodes does so in one relation as readily as in
  twenty.
