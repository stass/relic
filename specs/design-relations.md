# relic relation design

**Status.** Design document for the relation store: where a predicate's tuples
live, how a tuple is found, what makes an insertion report newness, and what
the tuple and memory limits are enforced against. Forward-looking, like every
document under `specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 20, 21, 24, 47 and 63;
`specs/design-facts.md` for the base stores and the commit that fills them;
`relic-plan.md` decisions D-3, D-4, D-6, D-7 and D-10.

---

## 1. What this document settles

Six questions, once, so that the planner, the evaluator and the index layer
answer none of them again:

* where a predicate's tuples live and what owns that storage;
* what a tuple identifier is, and what a consumer may assume about it;
* how a duplicate is recognized, and what "newly inserted" means;
* how a delta is represented, which is what semi-naive evaluation rests on;
* what the tuple and memory limits count, and when they are checked;
* what the arity-zero relation does, without a case of its own.

It does not cover secondary indexes over argument-position subsets. Those are
chosen by a plan, built lazily, and specified in `specs/design-index.md`;
which ones a plan asks for, and the statistics a relation offers the planner,
are in `specs/design-planner.md`. What this document specifies is the one
index every relation has: the canonical index over the whole tuple, which
exists to answer the membership question that duplicate elimination and
fixed-point detection are built on.

---

## 2. Where relations live

A predicate has two relations at most, in two tables: its **base store**,
which holds the facts the program states for it (`specs/design-facts.md`),
and its **derived relation**, which exists once a rule derives it and holds
what evaluation puts there. Each table is a prefix of the predicate table:
the relation of predicate `id` is entry `id - 1`, and the table is extended
to cover a predicate the first time a relation for it is asked for.

**Two functions hand them out, one per role.** The relation a rule writes is
the predicate's derived relation, created empty on first use; it is asked
only for a rule's head, and asking it for a predicate no rule derives is an
internal error rather than a relation created on the spot, so no rule can
write a base store. The relation an operation reads is the derived relation
of a derived predicate, and otherwise the base store of the predicate the
read names -- its own, or, for a base view `p$base`, the one it views --
which is then the whole relation; a base store is created empty on first
use, so a predicate nothing states or derives reads as empty. Below the two
functions nothing asks which kind a relation is, except to say what it is
called. What handles facts as facts rather than reading them through a plan
-- a load's commit, seeding, explain's fact count, the question of whether
the relations are current -- reaches a base store directly, by predicate
identifier.

The relation refers to its predicate by identifier rather than by pointer.
The predicate table is a growable array, so a pointer into it would dangle the
next time a predicate is interned.

**The tables hold pointers to relations, not relations.** The same growable
array underneath poses the same hazard in the other direction, and this is
where it has to be answered rather than warned about. A relation is created
once and then handed out repeatedly: a plan resolves the relation of each body
literal, an evaluation holds the head's relation across a whole rule, and both
outlive the creation of other relations. Were relations stored inline,
extending a table would move every one of them, and every holder would have
to know not to keep a pointer across a call that might create a relation --
correctness rested on a list of call sites that grows with every caller. Each
relation is therefore its own allocation, so growth moves pointers and never
relations, a relation's address is fixed for the life of the context, and
there is no rule for a caller to obey.

Relations are database state, not program structure. Keeping them in tables
of their own rather than in a field of `relic_pred_t` is what lets the derived
database be discarded without touching the program that derived it, which is
the operation a load's commit and `relic_fact_add()` need after an evaluation.

**One relation lives outside the tables: a query's answer.** It belongs to no
predicate and has no identifier to be indexed by; it is created with the
query object, emptied at the end of every run, and released with the query,
per `specs/design-query.md`. To the store it is a relation like any other --
the same struct, inserted into, read and emptied by the same functions,
counted against the same limits -- and only its ownership differs.

Discarding what was derived is safe because of who writes what. The
evaluator inserts into derived relations only, through a rule's `EMIT` and
by seeding a derived relation from its predicate's base store, so the next
evaluation seeds and derives everything a discard removes again. A load's
commit appends to base stores and nothing else does, and no discard touches
their tuples. A fact a caller adds is therefore committed to its base store
as a loaded fact is, never inserted into a derived relation: a tuple
inserted there would be erased by the next discard, and would not change
the base stores' tuple total that tells the evaluator the program changed,
so the next query would not see what it implies.

A relation holds its tuples, their canonical index, the secondary indexes of
`specs/design-index.md`, and the cached statistics of
`specs/design-planner.md` section 3.1, and no arena memory:

```c
struct relic_relation {
	relic_dom_t		*columns;	/* count tuples, arity apiece */
	relic_idtab_t		 index;		/* full tuple -> tuple identifier */
	relic_index_t		*indexes;	/* secondary, one per key mask */
	size_t			 count;
	size_t			 column_capacity;
	size_t			 index_count;
	size_t			 index_capacity;
	relic_relation_stats_t	 stats;
	relic_pred_id_t		 pred;
	uint16_t		 arity;
};
```

Every array is a growable array of `specs/design-memory.md` section 5, per
D-6: they outlive individual queries, they are the allocations large enough
to be worth accounting against a limit, and an arena cannot resize a block.

The arity is copied from the predicate rather than read back through it on
every insertion. This is not a second copy of a fact that can drift, because
the arity is half of the predicate's identity: `foo/1` and `foo/2` are two
predicates, so nothing can change the arity of one.

---

## 3. The tuple store

Tuples are stored in one flat array of domain identifiers with the arity as
the stride, so the tuple with identifier `id` occupies

```text
columns[(id - 1) * arity .. id * arity)
```

A tuple is therefore four bytes per column whatever its values are, and no
tuple has a header, a length or a pointer of its own. D-3 explains why the
column is an interned identifier rather than a value: a join, a hash and a
duplicate check are integer operations that never inspect a payload.

The capacity is counted in columns rather than in tuples so that one growth
policy serves every arity, the empty one included. Capacity is asked for in
columns, the growable-array helper doubles it, and the arity does not enter
the arithmetic a second time. An array that reaches a mebibyte gets a mapping
of its own, so a generation it outgrows goes back to the system at once
rather than staying resident (`specs/design-comparison-cost.md` section 8).

The number of columns a relation needs is `count * arity`, and that product is
checked before it is formed. A 32-bit tuple identifier space and an arity of
at most 32 put it inside a 64-bit `size_t` but not inside a 32-bit one.

---

## 4. Tuple identifiers and the append-only invariant

A tuple identifier is a 32-bit integer assigned from the relation's tuple count
in insertion order, starting at one. Zero names no tuple, which is the
convention every identifier in relic follows and which is what lets an empty
slot in the identifier table mean "empty" with nothing beside it.

A relation therefore holds at most `2^32 - 2` tuples. The ceiling is checked
rather than assumed: an identifier that wrapped would silently make one tuple
into another.

**The store is append-only.** A tuple identifier, once assigned, denotes the
same tuple until the database is modified. Nothing compacts, deletes,
reorders or renumbers it in between, which is what an evaluation needs: a
fixed point runs entirely inside one such span.

This is not a rule that callers observe; it is a property of the interface.
The relation store offers insertion, a load's reservation and append into
base stores, membership, reading a tuple by identifier, and two operations
that remove anything, both of which empty a relation whole: **clearing one
relation**, which a query's answer undergoes at both ends of a run, and
**discarding everything derived at once**, which empties every derived
relation and drops the base stores' secondary indexes. The evaluator
discards when the program has changed since the relations last reached its
fixed point, and a load's commit when it is about to add a fact. Nothing
removes, reorders or renumbers a single tuple, and nothing removes a base
store's tuples at all. A discard ends one span and begins another --
identifiers of a derived relation restart from the first, so a range taken
before it names nothing, or other tuples, after it, and a secondary index
taken before it is gone -- and the obligation it takes on is to make sure no
range outlives the span it was taken in. The evaluator meets it by
construction: every range lives in an evaluation's state, and a discard runs
only outside an evaluation, before one begins or in a load's commit.

A discard empties each derived relation in place and releases its columns
and indexes, and releases each base store's secondary indexes. The relations
themselves stay where they are, so a pointer to one remains valid and names
the same relation. Each derived relation is emptied through
`relic_relation_clear()`, and each index released gives back what it
counted, so the totals are at every moment the sum of what every relation
holds, in a table or not. A query's answer relation is outside the tables
and empty whenever a discard runs, per `specs/design-query.md` sections 3
and 6, so both totals count the base stores alone afterwards, which an
assertion confirms.

The store records whether it may hold anything a discard removes: the mark
is set where it hands out a derived relation to be written or builds a
secondary index, and cleared by the discard. A discard with the mark clear
has nothing to do and returns without walking the tables, so a run of loads
or `relic_fact_add()` calls between two queries pays for one discard.

D-10 is what rests on it. Semi-naive evaluation represents the tuples derived
in one iteration as a half-open range of tuple identifiers rather than as a
separate relation, so a delta needs no copying and no second store. Any future
incremental insertion or DRed-style deletion support has to confront this
first: the moment something can renumber a relation, a range captured before
it ran means something else afterwards.

---

## 5. The canonical full-tuple index

Each relation carries one index whose key is the whole tuple and whose value is
a tuple identifier. It answers the membership question, which is what
duplicate elimination, `MEMBER` execution and fixed-point detection all reduce
to.

It is the open-addressing identifier table of `specs/design-memory.md`: a slot
holds a tuple identifier and nothing else, and the key stays where the store
already keeps it. An entry therefore costs four bytes and no tuple is stored
twice.

**Hashing is over values, not bytes.** A tuple is hashed by folding its
columns as 32-bit words, not by hashing the bytes of the array. A byte hash of
a `uint32_t` array probes in a different order on a big-endian machine than on
a little-endian one, and D-4 requires that identical input produce an identical
probe order everywhere.

**A load's hash is carried in.** Every relation and staging set of a context
hashes a tuple with the context's seed and the same function, so the hash a
load's staging kept for a fact is the one its base store would compute.
`relic_relation_contains_hashed()` and `relic_base_append_hashed()` take that
hash rather than compute it, and a fact is hashed once on its way in
(`specs/design-facts.md` sections 4.2 and 4.3). A debug build computes each
hash again and asserts that it matches.

**Equality is a column loop, not a byte comparison.** The columns of two
tuples are compared as identifiers. A byte comparison would be equivalent for
a dense array of `uint32_t`, which has no padding, but it is not defined for
the empty tuple: `bcmp(NULL, NULL, 0)` is a comparison of zero bytes at a null
pointer, which the standard does not permit and a sanitizer reports. The loop
is defined for every arity and needs no case for the arity that would break the
other form.

---

## 6. Insertion returns newness

```c
int
relic_relation_insert(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *columns, bool *addedp);
```

`*addedp` is true only when the tuple was not already present. That single bit
is what fixed-point detection is built on: an iteration that added no tuple to
any relation has reached the fixed point, and an iteration is not required to
compare relations, count them or sort anything to find out.

Newness is an output rather than the return value because an insertion can
also fail -- for memory or for a limit -- and a two-valued return would have to
conflate a failure with a duplicate. Those are opposite answers: a duplicate
means the fixed point is closer, and a failure means the answer is unknown.

The insertion is ordered so that a failure leaves nothing behind:

1. **Look the tuple up.** A tuple already present is reported as not added and
   nothing is allocated.
2. **Check the limits** against what the relation and its indexes will hold
   once the tuple is in them. This is where the tuple limit is enforced on
   what evaluation inserts, as a load's commit enforces it on the facts it
   adds (below); the memory limit is enforced here, by the same projection
   when an index is built, and in the commit; each time before anything is
   modified.
3. **Reserve room in the index**, which is the step that can fail.
4. **Reserve room for the columns**, and in every secondary index, which are
   the other steps that can fail.
5. **Append the columns, place the identifier, and add the tuple to every
   secondary index.** None of these can fail.

Collecting every fallible step into one stage, ahead of every step that
changes what the relation holds, is what makes a failed insertion safe. A
tuple stored but not indexed is not a lost tuple: it is a tuple that
membership cannot find, so the next insertion of the same columns would append
a second copy, and a relation that contains a duplicate is no longer a set.
Every count, every join and every fixed-point decision downstream would be
wrong, and nothing would report an error. Because storing the columns and
placing the index entry both come after the last thing that can fail and
neither can fail itself, that state is unreachable rather than merely avoided.

A failure therefore leaves the relation's contents and its index agreeing with
each other and holding what they held. It may leave behind capacity that a
half-finished reservation obtained; that capacity is accounted, and the next
insertion uses it.

`columns` holds identifiers the caller has already interned. The store does
not validate them, because it never inspects a value: a column is an opaque
32-bit integer to everything in this layer.

```c
int
relic_relation_insert_batch(relic_t *ctx, relic_relation_t *relation,
    const relic_dom_t *tuples, size_t count, bool *added,
    size_t *completedp);
```

inserts `count` tuples packed at the relation's arity exactly as that many
single insertions would in order, reporting each one's newness in `added`
and how many completed before any refusal in `*completedp`. It exists so
that the cache misses of the duplicate checks overlap rather than follow
one another, `specs/design-evaluation-cost.md` section 6. Both entry points
hash the tuple and then perform one insertion by that hash, the steps
above, so they cannot disagree about what a duplicate is or where a limit
falls.

**A load's commit reserves, then appends.** The facts a load adds to base
stores are not inserted one at a time: the commit has looked every staged
fact up already, and knows how many each base store gains
(`specs/design-facts.md` section 4.3). The store checks both limits once,
against the totals plus everything the commit adds, and refuses the whole
load before any store is created or grown. It then reserves room for *n*
tuples in each store, growing its columns and its canonical index at most
once, to exactly the bytes the check projected, and the commit appends each
fact into that room without looking anything up; no append can fail. A
reservation for more than one tuple is made only of a relation without
secondary indexes, which a base store is when a commit reserves, since the
commit discards what was derived first: a secondary index would be given
room for a key per tuple, far more than a column of few values uses. A
failed allocation among the growths leaves every store's contents as they
were, and keeps whatever capacity it obtained, accounted.

---

## 7. The empty tuple

A relation of arity zero holds either no tuple or one tuple, and it reaches
that behaviour through the general path.

The key of its one tuple is the empty sequence of columns. The hash of an
empty key is defined, the comparison of two empty keys is true, and the stride
of an empty tuple is zero, so the second insertion finds the first and reports
that nothing was added. There is no case for arity zero anywhere in the
store.

The one visible consequence is that the column array of such a relation is
allocated at its minimum size and holds nothing, because the growable-array
helper does not distinguish a request for no items from a request for one. The
cost is a few dozen bytes per arity-zero relation and buys the absence of a
special case.

---

## 8. Ranges

Iteration over a relation is iteration over a half-open range of tuple
identifiers:

```c
struct relic_tuple_range {
	relic_tuple_id_t	first;	/* inclusive */
	relic_tuple_id_t	end;	/* exclusive */
};
```

A range is a value, not a cursor: it holds no pointer into the relation, so a
growth during iteration cannot invalidate it, and a consumer reads each tuple
through the relation in the ordinary way.

Two of them are the whole interface. `relic_relation_all()` is every tuple, and
`relic_relation_since(relation, mark)` is every tuple added since `mark` was
taken with `relic_relation_end()`. The second is the delta of D-10, written as
what it is rather than as two loose integers whose meaning the reader has to
recover.

Iteration order is insertion order, because a tuple identifier is assigned in
insertion order. D-4 requires it: a relation iterated in hash order would make
output depend on a hash seed and a growth history.

---

## 9. Limits and accounting

Two of the four configurable limits of D-7 are enforced here, because tuples
and indexes are what actually grow:

| Limit | Counts | Configured by |
| --- | --- | --- |
| `max_tuples` | tuples across every relation, base stores included | `-l tuples=N` |
| `max_memory` | bytes of column and index storage, plus an operation's arenas | `-l memory=N` |

Zero means unlimited, so a zeroed configuration asks for the defaults.

**What the memory limit counts** is the bytes the column arrays, the canonical
index's slots and every secondary index's arrays actually hold, summed over
every relation, base stores and derived relations alike -- allocated
capacity, not the smaller number a tuple count implies. A limit that ignored
the difference would let a relation hold twice what the user allowed, which
would defeat the purpose of asking. A fact of a derived predicate is held
twice, in its base store and in its derived relation, and counted twice.

It does not count the arrays of relation headers, a relation's array of index
headers, the predicate table, the value table, the arenas that hold the
program's rules, the schedule or a query as the caller wrote them, the text
a load reads, or the staging a load holds its facts in until it commits.
Those are sized by what the caller wrote, and the limit exists to bound what
the program's stores hold and what running the program can do. The arenas
that hold an evaluation's plans and a query's rewrite are sized by the square
of a body and are charged to the same total, per
`specs/design-planning-cost.md` section 6; the check there is the arena's,
and this document's total is the part the store contributes.

**When it is checked** is before the insertion, or a load's reservation,
modifies anything. The
projected size is computed from the growth policies rather than measured after
the fact, so that a limit which fires has prevented the allocation rather than
reported it. Two pure functions make that possible: one gives the capacity a
growable array would reach, the other the bytes an identifier table would hold
after a reservation. Each is also what the corresponding growth itself uses,
so the prediction and the policy cannot drift apart.

**How the total is maintained** is by reconciliation rather than by
bookkeeping at each site. An insertion measures what the relation holds,
reserves, and adds the difference to the total. That stays exact whether the
reservation grew both arrays, grew one and then failed, or grew nothing, so no
failure path has to remember to give anything back. A total incremented where
allocations succeed and decremented where they fail would be a list of places
to remember, and a step added later would not be on it.

**When a limit fires**, the relation is exactly as it was. The check precedes every
mutation, so consistency after a refused insertion is a property of the
ordering rather than of an unwind path. The diagnostic names the limit, its
configured value, the activity and the relation, in the form of architecture
section 63: the predicate being derived into, the one being indexed when the
memory limit fires while an index is built, or, when a load's commit is
refused, the first predicate, in the order the load's facts name them, whose
base store it would grow. The activity is a parameter of the one function
that reports a refusal, so every limit is reported in the same form:

```text
relic: limit exceeded: tuples=1000000 while deriving path/2
relic: limit exceeded: memory=536870912 while deriving path/2
relic: limit exceeded: memory=536870912 while indexing edge/2
relic: limit exceeded: tuples=4 while loading e/1
```

A relation is named by its predicate, and the base store of a derived
predicate by its base view's name, `p$base`, since `p` names the derived
relation.

Discarding the partial results is the evaluator's part and is specified with
it. The store's part is to refuse the tuple and to stay consistent.

---

## 10. Determinism

The relation store contributes three things to the reproducibility of a run:

* **tuple identifiers are assigned in insertion order**, and iteration is by
  identifier, so no result is ordered by a hash;
* **the tuple hash folds 32-bit words**, so for one seed the probe order is
  the same on every platform; the seed itself is drawn per context, per
  `specs/design-hash-seed.md`, and decides nothing the relation reports;
* **nothing is keyed on an address**, and a relation's contents do not depend
  on how many times its arrays have grown.

---

## 11. What is deliberately absent

* **Removal of a tuple.** Nothing deletes one tuple; the only removal is the
  discard of every derived relation, which is what keeps the append-only
  invariant a property rather than a rule, and a base store loses nothing.
  The identifier table underneath the index has no tombstones for the same
  reason.
* **A sorted or tree-shaped store.** Membership is the operation that matters
  and a hash answers it in constant time; sorting is an output concern and is
  done by the output layer on the rows it is about to print.
* **Statistics maintained on insertion.** Distinct counts are computed when
  the planner asks and cached against the tuple count, per
  `specs/design-planner.md` section 3.1.
* **A per-relation memory limit.** The limit that protects a user is the total,
  and a program that explodes does so in one relation as readily as in twenty.
