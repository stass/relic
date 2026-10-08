# Facts

The facts a program states are stored as the tuples of a per-predicate
**base store**, with nothing per fact in the syntax tree, the rule table or
the schedule. This document covers how a load, an import or
`relic_fact_add()` stages and commits facts all or nothing, how evaluation
seeds derived relations from them, how what was derived is discarded when
the program changes, when the relations are current, and how the Magic
rewrite reads facts through the base predicate `p$base`. Facts are a
foundation shared by two pipeline stages: the
[front end](design-frontend.md), which stages them, and
[evaluation](design-eval.md), which reads them; what a fact means is in
[the language reference](language.md#programs).

Related: [architecture: facts](architecture.md#facts),
[front end](design-frontend.md), [storage](design-storage.md),
[evaluation](design-eval.md), [Magic Sets](design-magic.md),
[tables](design-tables.md), [memory](design-memory.md).

## Model

A fact is a tuple of a few words. Held as a program object it would also be
a statement with a term and location per column, a compiled rule, and a
schedule entry, and the check and the dependency analysis would handle it
as a rule: several hundred bytes and most of the loading time per fact,
against a tuple and an index slot. Nothing about a fact needs the rule
machinery -- no body to plan, no variable to bind, no dependency to order.
It needs to be a member of its predicate's relation whenever that relation
is read, and a base store is exactly that.

```text
load text / import / relic_fact_add
        |
     staging     per predicate, deduplicated, measured against the limits
        |
     commit      discard -> reserve -> append
        |
   base store of p --seed--> derived relation of p   (only if p is derived)
```

A predicate has up to two relations:

* its **base store**, an ordinary relation ([storage](design-storage.md))
  in a table of its own indexed by predicate identifier. Written only by a
  commit, it only grows and is released with the context.
* its **derived relation**, which exists only for a **derived** predicate,
  one with a rule appended to a rule table
  ([predicate identity](design-frontend.md#predicate-identity)).
  Evaluation writes it; a change to the program discards it.

An aggregate's group predicate is derived and has no base store: its name
holds `$`, so no fact can be stated for it.

## Invariants

* Only a commit writes a base store, and base stores only grow.
* No rule can write a base store.
* A discard removes everything derived -- derived relations and the base
  stores' secondary indexes -- and never a base store's tuples.
* The relations are current exactly when the recorded pair (rule count,
  base-store tuple total) equals the program's; both counts only grow.
* A commit adds every new tuple of its load or changes no base store's
  contents.

## The read and write funnel

Two functions take a predicate to the relation a plan uses:

* **the relation a rule writes** is the derived relation, created empty on
  first use. Asking it for a predicate that is not derived is an internal
  error, not a relation created on the spot, so no rule can write a base
  store and no mistake can give a predicate a second relation.
* **the relation an operation reads** is the derived relation of a derived
  predicate, and otherwise the base store of the predicate the read names --
  its own, or for the base predicate `p$base` the store of `p`. A base store is
  created empty on first use, so a predicate named only in a query reads as
  empty.

Where facts are handled as facts -- the commit, seeding, explain's fact
count, the counters, the currency total -- the base store is reached
directly; none of these writes a derived relation or reads one in place of
a base store, so none needs the funnel's choice.

A predicate of facts alone is read straight from its base store and holds
its facts once. A derived predicate with facts holds them twice, in its
base store and in its derived relation, which is seeded from it: a tuple
and an index slot more per such fact, the price of a discard that never
has to separate facts from what was derived. A predicate becomes derived
when its first rule is appended, which moves the rule count, so the next
evaluation discards and seeds afresh and no plan keeps a relation that
stopped being the one its predicate is read through.

## Staging

A load's staging holds one **staged set** per predicate its facts name, in
the order the load first names them: the load's distinct facts packed at
the arity, an identifier table making them a set, and per tuple its hash
and a "new" mark. A fact finds its set through the set the previous fact
used, which a file of one predicate's facts always hits, and otherwise
through a small table keyed by predicate.

**A tuple's hash is computed once**, under the context's seed and exactly
as its base store would compute it; the set's table, the base-store lookup
and the commit's append all reuse it. A debug build recomputes it at the
append and asserts it matches, checking what a release build trusts.

**Staging measures each fact against the limits.** Staging looks each
tuple the set lacks up in its base store, an absent store counting as
empty, and marks it new or not. Nothing can change a base store while a
load runs, so the commit trusts the mark. A new tuple is staged only if the
load, with it, fits both limits:

* **tuples**: the base stores' facts when the load began plus the new
  tuples, within `max_tuples`;
* **memory**: a floor under what the commit is to hold -- the value table
  and symbol pool as they stand, plus each loaded predicate's base-store
  columns at its count and new tuples -- within `max_memory`.

The floor is safe to refuse on. The value table and pool only grow during
a load, and the commit discards derived relations before it reserves, so
the floor never exceeds what the commit measures for any growth the stores
can hold: staging refuses no load the commit would admit. The floor omits
canonical indexes, whose growth depends on how tuples fall into stripes;
the commit measures them and can refuse what the floor admitted.

A refusal ends the load at the fact that shows it, since the parser stages
a fact before reading the next token and an import stops at the record. The
load then stages and interns no more of its input, and since nothing in the
program changed, the derived relations survive. Staging bounds the parse,
not the read: a file is read whole first, and an import may have read a
buffer past the record that ends it.

**Staging is charged to no limit.** It is the load's working storage,
proportional to the text as the text and program arena are, bounded by the
limits only through the new facts they admit, and gone when the load
returns. Charged, it would count against the limit alongside the derived
relations the commit is about to discard, and together they could refuse a
load that fits without them. Its tables hash with the context's seed, since
the text is untrusted. One function releases staging, on the load's only
exit.

## The commit

When the input has been read, the load commits its facts, then splices its
statements ([loading](design-frontend.md#loading)). A load with no new
tuple commits nothing. Otherwise the commit first computes each store's
growth, which can fail for memory and then changes nothing, and then:

1. **Discard.** The program is about to change, so what was derived is
   discarded ([the discard](#the-discard)). It cannot fail, and it leaves
   only the base stores' columns and canonical indexes, so the next step's
   limits count the program's facts and nothing derived.
2. **Reserve.** Compute the new tuples and the bytes each base store's
   columns and canonical index occupy at its new count; check both limits
   once against the account plus those; then create absent stores and grow
   each. A refusal reports
   `relic: error: limit exceeded: tuples=N while loading p/2` (or
   `memory=N`), naming the first predicate, in the load's order, whose store
   would grow. Every base store's contents and the program stay as they
   were; a store that grew before a failed allocation keeps its capacity,
   accounted, as any relation that failed to grow does.
3. **Append.** Append each set's new tuples in staged order, placing each
   in the canonical index by its staged hash. Room is reserved, every tuple
   is one the store lacks, and no secondary index remains to be linked, so
   no append looks anything up and none can fail.

**No step needs an unwind.** Every fallible step precedes the first change
a reader can see. Truncating stores a refused commit had begun to fill
would make consistency after a refusal the work of an unwind path; ordering
every fallible step first avoids it at the cost of one read-only lookup per
staged fact. A refused commit has discarded the derived relations, which
the next query evaluates again.

**`relic_fact_add()` is a load of one**: it validates its arguments,
stages and commits, so the fact is in its base store when the call returns
and nothing waits for the next check. A run of calls between queries pays
for one discard, since a discard with nothing derived returns at once.
**An import is a load whose text is a table** ([tables](design-tables.md)):
it stages each record as read, measured as a load's facts are, and commits
once at the end. Duplicates alone change neither count and leave the
relations current.

## Seeding

Before a component's first pass -- round 0 of a semi-naive fixed point, the
first naive pass, or a non-recursive component's single pass -- each
member whose base store holds tuples has its derived relation seeded with
the whole store, in identifier order, in batches as a plan's output is
inserted. Seeding precedes the recording of the first deltas, so every
seeded tuple is part of round 0's gain. It counts a derivation per base
tuple and an insertion per tuple the derived relation lacked, so
`derivations` and `tuples inserted` count evaluation, seeding included,
while a commit counts in neither.

A body-less rule inserts its head when its turn comes, after confirming
every argument is a constant. The program's rule table holds none, since a
ground fact goes to its base store and a fact with a variable is rejected;
a rewrite's table holds its seeds, such as `magic$p$bf(a).`, inserted on
every evaluation of the rewrite.

## The discard

Every discard goes through one function: the commit's, whole-program
evaluation's when the relations are not current, and each Magic run's before it
derives. It frees every derived relation, drops the base stores' secondary
indexes and clears the record of what was evaluated; base stores' tuples and
canonical indexes stay. An index over a base store therefore lives exactly as
long as one over a derived relation, and the next evaluation rebuilds what it
asks for.

The store keeps a mark that it may hold something derived, set where a
derived relation is handed out and where a secondary index is built -- the
only ways to create the two things a discard removes -- and cleared by the
discard. With the mark clear a discard returns without walking anything. A
relation pointer, tuple range or index position taken before a discard
names nothing after it; the evaluator discards only before an evaluation
begins, and every range and plan dies with its evaluation.

Discarding everything rather than continuing from stale relations is
deliberate: continuing would be sound without negation or aggregates, where a
new fact can only add, but not with them, where a new fact can make a derived
tuple false. One rule for every program keeps the question from arising.

## Currency

A program changes only by gaining rules or base-store tuples, and both
counts only grow. A successful whole-program evaluation records the
pair -- rule
count, base-store tuple total -- and the relations are current exactly
when a pair is recorded and neither count has moved. A discard clears the
record, so a failed evaluation leaves the relations stale and the next
starts again from the facts. A Magic run records nothing, since it derives
only part of the fixed point. One function answers the question for every
caller.

## The base predicate

The Magic rewrite reads a derived predicate's facts apart from what its
rules derive, so a base store can be named as a predicate of its own: **the
base predicate `p$base`**, of `p`'s arity, whose entry names `p` as the
predicate whose base store it reads. No rule derives it, and no fact can be
stated for it, since its name holds `$`. The read side of the funnel
resolves it to `p`'s base store, which a plan looks up by its bound columns
like any relation, so a read of it reaches only the facts a demand names.
How the rewrite uses it -- as the first rule of each adornment, so a derived
predicate's rows begin with its facts and the rewrite's size does not
depend on how many facts there are -- is in [Magic Sets](design-magic.md).

## Schedule, explain and counters

The [schedule](design-stratify.md#the-schedule) lists components with at
least one rule, so a predicate of facts alone is in none, and loading facts
changes neither the dependency graph nor the strata. Explain prints
`facts: N` under a component, counting its body-less rules and its members'
base-store tuples, when that is not zero; components are numbered without
the fact-only predicates. `tuples held` and `storage bytes` count base
stores and derived relations alike, a fact held in both counting twice.

## Failure paths and limits

* The tuple and memory limits count every store, base stores included, and
  refuse a load at staging or at the reserve, `while loading`.
* A staging refusal changes nothing and keeps the relations current; a
  reserve refusal leaves base stores and program as they were, with the
  derived relations discarded.
* A load holds its staging and its reserved room at once, about twice the
  new facts' storage, until it returns; only the room is charged.
* A derived predicate with facts reaches a limit sooner for holding them
  twice.
* What a failed load, import or fact addition interned stays interned,
  holding nothing and referenced by nothing.

## Determinism

A load interns predicates and values as it reads them, and the commit
appends tuples in staged order; seeding inserts a base store in identifier
order before the first pass; a component's rules then run in program order.
The same input therefore gives the same identifiers, plans and row order,
and the rows of a derived predicate with facts begin with all of its facts,
under whole-program evaluation or under the rewrite.

## Alternatives not taken

* **Incremental insertion.** A fact added after evaluation discards what
  was derived rather than extending it. A new base tuple is exactly a delta
  of an extensional predicate, so the base store is what incremental
  maintenance would build on, but with negation or an aggregate a new fact
  can make a derived tuple false, and maintaining that needs deletion and
  rederivation through every stratum above it. Discarding is one rule for
  every program and costs one evaluation per change.
* **Retracting facts.** Nothing retracts a fact; a base store has no
  deletion.
* **Sharing a base store with its derived relation.** Facts would be held
  once, but a discard would have to cut derived tuples out of a store that
  must keep the facts.
* **A fact representation in the tree.** Facts would stay in the program's
  pipeline and every evaluation would copy them into the store.
* **Committing an import in batches.** It would bound staging, but a failed
  import would leave earlier batches committed, breaking all or nothing.
