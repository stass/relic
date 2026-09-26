# relic facts design

**Status.** Design document for how a program's facts are stored: as the
tuples of a per-predicate base store, with nothing per fact in the syntax
tree, the rule table or the schedule. In the language a fact is a rule with
an empty body; this document settles where the facts a program states are
kept, and nothing about what they mean. Forward-looking, like every
document under `specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/design-evaluation-cost.md` section 7, which traces
the cost of loading facts to their representation; `specs/design-parser.md`
sections 5 and 8, the tree and the load; `specs/design-program.md`, the rule
table and predicate identity; `specs/design-relations.md`, the store a base
store is; `specs/design-stratify.md` and `specs/design-eval.md`, the
schedule and the first round; `specs/design-magic.md`, the rewrite of a
predicate derived by facts and rules alike; `specs/design-api.md` section
4, `relic_fact_add()`; `specs/design-memory.md`, the account; architecture
sections 5.1, 54, 69 and 70.

---

## 1. What this document settles

* **the base store**: the facts a program states for a predicate are the
  tuples of that predicate's base store, a set that no change to the
  program discards;
* **derived predicates**: a predicate is derived exactly when a rule with
  it as its head has been appended to a rule table, a mark set in one
  place, which decides the relation the predicate is read and written
  through;
* **the commit**: a load parses its facts into staging and commits them all
  or nothing, measuring and reserving before it adds anything;
* when a program is current, as a pair of counts that only grow;
* the first round, the discard, Magic, explain, the counters and the
  limits, with the facts in base stores;
* the tests and the order of work.

A body-less rule is a rule. The Magic rewrite generates body-less rules,
its seeds, and the evaluator runs them as it runs every rule; the facts a
program states are held in base stores, and the rule table holds its
rules.

---

## 2. Why

A fact stated in a program is a tuple of a few words. Held as a program
object it is also a statement with a term and a location per column in the
syntax tree, a compiled rule with an argument per column in the rule table,
and an entry in its component's schedule; and the check, the dependency
analysis and the stratification handle it as a rule. For a program of many
facts that is several hundred bytes and most of the loading time per fact,
against about twenty bytes of tuple and index slot.

Nothing about a fact needs the rule machinery: it has no body to plan, no
variable to bind and no dependency to order. It needs to be a member of its
predicate's relation whenever that relation is read. The base store is
exactly that.

---

## 3. Base stores and derived predicates

### 3.1 The base store

**The facts a program states for a predicate are the tuples of its base
store.** A base store is a `relic_relation_t`: columns, a canonical index
that makes it a set, and whatever secondary indexes plans ask for. Base
stores live in a table of their own, indexed by predicate identifier, are
charged to the tuple and memory limits as relations are, and are written
only by a commit (section 4.3). A program only grows and nothing retracts a
fact, so a base store's tuples only grow, and the store is released with
the context.

### 3.2 Derived predicates

**A predicate is derived when a rule with it as its head has been appended
to a rule table.** Both rule tables -- the program's, and the one a query's
rewrite builds -- append through one function, whose infallible last step,
after every allocation it makes has succeeded, stores the rule, advances the
count and marks the head predicate derived. Nothing else sets the mark and
nothing clears it. So every predicate a rule can write is derived, whichever
table the rule is in, and a predicate that is derived has had a rule
appended.

The mark outlives a rewrite's table, which is freed after its query. That
is harmless: a predicate only a rewrite derives is one the rewrite
generated, which nothing but a rewrite reads.

A derived predicate has a **derived relation**, which evaluation writes and
a change to the program discards.

### 3.3 The relation a predicate is read and written through

Two functions take a predicate to the relation a plan uses, one per role:

* **the relation a rule writes** is the predicate's derived relation,
  created empty on first use. It is asked only for the head of a rule, and
  asking it for a predicate that is not derived is an internal error
  rather than a relation created on the spot, so no rule can write a base
  store and no mistake can create a second relation for a predicate;
* **the relation an operation reads** is the derived relation when the
  predicate is derived, and otherwise the base store of the predicate the
  read names: its own, or, for a base view (section 3.4), the one it views.
  A base store with no tuples is created empty on first use, so a
  predicate that appears only in a query or a body reads as empty.

Where facts are handled as facts rather than read by a plan -- the commit,
seeding, explain's fact count, the counters and the currency total -- the
base store is reached directly, by predicate identifier. None of these
writes a derived relation or reads one in place of a base store, so none
needs the funnel's choice.

So a predicate of facts alone, like `facts-join`'s `row`, is read straight
from its base store, and its facts are held once. A derived predicate that
also has facts holds them twice, in its base store and in its derived
relation, which is seeded from the base store before its component's first
pass (section 5.1): a tuple and an index slot more per such fact, the price
of a discard that never has to separate facts from what was derived.

A predicate becomes derived when its first rule is appended, which is a
change to the program: the next evaluation discards every derived relation
and seeds from the base stores, so no plan holds a relation that stopped
being the one its predicate is read through.

### 3.4 Base views

The Magic rewrite reads a derived predicate's facts under a guard (section
6), so a base store can be named as a predicate of its own: `p$base`, of
`p`'s arity, whose predicate entry names `p` as the predicate it views. No
rule derives it, and the read funnel resolves it to `p`'s base store
through that entry. No identifier the lexer accepts contains `$`, and
`relic_fact_add()` rejects `$` in a name, so no program and no caller can
name or write it.

### 3.5 Defined and referenced

A predicate is **defined** when something derives into it: a rule's head
names it, its base store holds tuples, or it is a base view. The first is a
mark, set where a rule's head is compiled, before the rule's safety is
checked so that one mistake draws one complaint, and where the rewrite
interns a predicate it generates. The other two are read from the base
store and the predicate's entry when asked. `defined` is a function of the
three, so nothing that stores a fact has a mark to remember.

A predicate is **referenced** when an atom of a rule body or a query names
it, a mark set where every such atom is compiled, whether or not its
statement goes on to be accepted, and never cleared. When the mark is
first set it records the atom's location. The undefined-predicate warning
names a predicate that is referenced and not defined, and points at that
location. So a predicate a load interned for facts it never committed, or
that a rewrite generated and then abandoned, is referenced by nothing and
never named, and the warning depends only on what the program's rules and
queries read, and points only into text the program holds.

---

## 4. Loading

### 4.1 The parser stages a fact

A load parses into staging, and the statement a fact would have been is not
built. The parser reads a head's terms into its scratch array of
`RELIC_MAX_ARITY` terms rather than into the program arena, and then:

* followed by `:-`, the head is copied into the arena and the rule parsed
  as a statement;
* followed by `,` or `?`, the atom is a query's first literal, and it is
  copied into the arena and the query parsed as a statement;
* followed by `.`, with every term a constant, the fact is added to the
  load's staging (section 4.2), and nothing is kept in the tree;
* followed by `.`, with a variable among the terms, the head is copied and
  kept as a statement, and the check rejects it as it rejects any unsafe
  rule, with the diagnostic an unsafe fact receives: `variable X in fact
  edge/2 is not bound by a positive body literal`. A fact that cannot be a
  tuple is an error, and keeping its statement keeps that error exact. A
  rejected statement is never appended to the rule table.

Only a load stages. The parser also reads a query's text, where a fact is
an error; there it keeps a fact as a statement, and the query parser
reports it as a fact where a query was expected.

### 4.2 Staging

A load's staging holds a staging set per predicate its facts name, in the
order their first facts appear. The parser interns a fact's predicate as it
reads the fact, as the lexer interns the name: a predicate a failed load
leaves interned holds nothing and is referenced by nothing, which is what
an unused identifier is (section 3.5). A staging set holds the load's
distinct facts of its predicate, packed at its arity, an identifier table
over them that makes it a set, and a mark per tuple that the commit sets.
A fact finds its set through the set the previous fact used, which a file
of one predicate's facts always hits, and otherwise through a small
open-addressing table keyed by predicate identifier.

Staging is the load's working storage rather than the program's. Its
packed tuples grow by doubling, and its identifier tables are allocated as
every identifier table is and hash with the context's seeded hash, since
the text is untrusted. Staging is charged to no limit. It is proportional
to the text being loaded, as the text itself and the program arena are,
which no limit bounds either, and it is gone when the load returns;
charged, it would count against a limit while the derived relations the
commit is about to discard still did, and they could refuse a load that
fits without them. A failed allocation is reported as the load's arena
exhaustion is. One function releases staging, and it is the load's only
exit: a load that fails to parse, one the commit refuses and one that
commits all end there.

### 4.3 The commit

A load is all or nothing in what it adds to the program,
`specs/design-parser.md` section 8. When the whole input has parsed, the
load commits its facts, then splices its statements:

1. **Measure.** For each staged tuple, look it up in its predicate's base
   store, an absent store counting as empty, and mark the tuple new or
   not. Every mark is written here, and the append trusts them. Count the
   new tuples of each set. Nothing but the marks changes, and nothing can
   fail.
2. **Discard.** If any tuple is new, the program is about to change, so
   what was derived from it is discarded (section 5.3): the derived
   relations, the base stores' secondary indexes, and the record of what
   was materialized. The discard cannot fail, and it leaves the store
   holding exactly the base stores' columns and canonical indexes, so the
   limits of the next step count the program's facts and nothing derived.
3. **Reserve.** Compute what the load adds: its new tuples, and the bytes
   each base store's columns and canonical index occupy once grown for its
   new count, an absent store counting as empty. Check both limits once,
   against the account's totals plus those. A refusal reports `limit
   exceeded: tuples=N while loading p/2`, or its `memory` form, naming the
   first predicate, in the order the load first names them, whose store
   would grow, and the load fails with
   nothing grown: the base stores' contents and the program are as they
   were, and the derived relations are discarded, to be evaluated again by
   the next query. Then create each absent store and grow each store for
   its new count. A failed allocation among these fails the load the same
   way, and a store that had grown keeps its capacity, accounted, as any
   relation that failed to grow does.
4. **Append.** Append each set's new tuples to its base store in the
   order they were staged, placing each in the canonical index. Room is
   reserved, every appended tuple is one the store lacks, and no secondary
   index remains to be linked, so no append looks anything up and none can
   fail.

### 4.4 relic_fact_add()

`relic_fact_add()` validates its arguments, then stages its one fact and
commits it as a load does. The fact is in the base store when the call
returns, with nothing left for the next check. A limit can refuse it,
which leaves the program as it was. A discard with nothing derived to
discard returns at once (section 5.3), so a run of calls between two
queries pays for one discard.

### 4.5 Identifiers and order

A load interns its fact predicates in the order their first facts appear,
before any rule of the same load is compiled, and a commit appends each
predicate's new tuples in the order they were parsed. Both depend only on
the text, so two runs over the same input intern and store alike.

---

## 5. Evaluation

### 5.1 Seeding

Before the first pass of a component -- round 0 of a semi-naive fixed point,
the first pass of a naive one, or the single pass of a component that is
not recursive -- each member with a base store that holds tuples has its
derived relation seeded from it: the whole store, in identifier order,
inserted in batches of the evaluator's batch size. Seeding precedes the
recording of the first deltas, so every seeded tuple is part of the first
delta as a gain of round 0. It counts a derivation per base tuple and an
insertion per tuple the derived relation did not hold. A store with no
columns seeds through a non-null buffer, as the batch insertion requires.

### 5.2 Body-less rules

A rule with no body inserts its head when its turn comes, after confirming
that every argument of its head is a constant. The program's rule table
holds none, since a fact is committed to its base store and an unsafe fact
is rejected before it is appended; a rewrite's table holds its seeds,
`magic$p$bf(a).`, which every evaluation of the rewrite inserts.

### 5.3 The discard

A changed program discards every derived relation and no base store, and
so does every other discard, whatever calls it, through one function: the
commit's, and the one that precedes each query's rewrite. The base stores'
secondary indexes are dropped with the derived relations, so an index a
plan asked for over a base store lives exactly as long as one over a
derived relation, and the next evaluation builds the ones it asks for
again. After a discard the store's accounted bytes are the base stores'
columns and canonical indexes. Every discard clears the record of what was
materialized (section 5.4).

The store records whether it may hold anything derived: the mark is set
where the write funnel hands out a derived relation, and where a secondary
index is built, the only two things a discard removes, and cleared by the
discard. It is set on every hand-out rather than on creation, since a
discard empties a derived relation and keeps it for the next evaluation.
A discard with the mark clear has nothing to do and returns without
walking the relations.

### 5.4 When a program is current

The relations are current when they were evaluated from the program as it
stands. A program changes by gaining rules or by its base stores gaining
tuples, both of which only grow, so materialization records the pair -- the
number of rules, and the base stores' tuple total -- and the relations are
current exactly when a pair is recorded and it has not changed. A discard
clears the record. One function asks the question, for every caller that
needs to know. A load of duplicates alone changes neither count and leaves
the relations current.

---

## 6. Magic

**Completeness.** The rewrite asks whether a predicate is complete only of
the program's predicates and of base views, all of which exist when it
takes its snapshot of the predicate table. A predicate is complete when it
is not derived, its base store then being the whole relation, or when its
component is complete; the rewrite reads a complete predicate as it is and
never adorns it.

**Base views are interned before the rewrite begins.** When the rewrite
finds the complete predicates, it interns the base view of every derived
predicate whose base store holds tuples, so each base view exists when the
snapshot is taken, and it is complete by the rule above.

**A derived predicate's facts enter each of its adornments as one rule
over its base view**,

```prolog
p$bf(X, Y) :- magic$p$bf(X), p$base(X, Y).
```

and not as a guarded copy of each fact. The rule is not the program's:
the rewrite adorns `p(X1, ..., Xn) :- p$base(X1, ..., Xn)`, built for the
purpose, whose one literal is complete and is left as it is, and it is the
adornment's first rule, so a derived predicate's rows begin with its facts
under the rewrite as they do when it is materialized. The planner looks the
base store up by the bound columns like any complete relation, so an
adornment reads only the facts its demand reaches, and the rewrite's size
does not depend on how many facts there are.

**The budgets** are proportional to the program, `specs/design-magic.md`:
each is 256 plus 16 per unit of the program.

| Budget | Unit of the program |
| --- | --- |
| generated predicates | a member of the program's schedule |
| generated literals | a rule's body length plus one |

Generated rules have a limit and no budget: the literal budget bounds
them, since every generated rule but a seed has a body literal, and every
seed but the query's follows a draft charged one. A derived predicate with
facts counts as though the program held the rule its adornments are given,
`p(X, Y) :- p$base(X, Y).`: two literals. The facts themselves count
nothing, since the rewrite copies none of them. A base view counts against
no budget or limit, since it adds no relation.

---

## 7. The schedule and explain

**The schedule is built from rules.** It lists the components with at
least one rule, and a predicate of facts alone is in none; it is read from
its base store wherever a rule or a query reads it. Loading facts adds
nothing to the dependency graph, the strata or the rule counts.

**Explain** prints a component's heading, then `facts: N`, where N counts
the component's body-less rules and the tuples its members' base stores
hold, when that is not zero, then its plans. A rewrite's seed components
count their seeds. A base store is a set, so N counts distinct facts. A
predicate of facts alone is in no component and is not printed, and the
components are numbered without it.

---

## 8. Counters and limits

* `derivations` and `tuples inserted` count evaluation: the rules'
  derivations, and the seeding of a derived relation from its base store.
  A commit is not evaluation and counts in neither.
* `tuples held` and `storage bytes` count the base stores and the derived
  relations alike, a fact held in both counted in both.
* The tuple and memory limits count every store, base stores included, and
  refuse a commit as section 4.3 describes, with `while loading`; the
  activity a limit names is a parameter of the store's refusal.
* A load needs its staging and the room its commit reserves at once, about
  twice the new facts' storage, until it returns; only the room counts
  against the memory limit (section 4.2).

A program's facts are refused when they are loaded, if they do not fit,
and a derived predicate with facts reaches a limit sooner for holding them
twice.

---

## 9. The tests

* **The relation store.** Reserving for *n* tuples grows the columns, the
  canonical index and any secondary index once, to exactly the bytes
  projected for it, charges them, and changes no content, and *n* new tuples
  then go in without growing anything; a count plus *n* past the largest
  identifier, or a size no `size_t` expresses, is refused without overflow
  and grows nothing. The identifier table reserves for *n* entries alike.
* **Loading.** A fact with a variable is kept as a statement and rejected by
  the check with its diagnostic, and draws no undefined-predicate warning; a
  predicate first named by a fact of a load that failed, then referenced by a
  rule, is warned about at the rule's atom; a query in a loaded file whose
  first literal is ground is kept as a query; a load that fails to parse
  commits no fact; a load refused by the tuple limit, and one refused by the
  memory limit, grow no base store, and leave the program, every base store's
  contents and the warnings as they were, and its derived relations discarded
  only when it held a new fact; a load of duplicates alone leaves the
  relations current; a fact in a query's text is reported as a fact where a
  query was expected.
* **The funnel.** Asking the write side for a predicate that is not
  derived is refused; a predicate named only in a query reads as empty; a
  predicate that gains its first rule after its facts were loaded answers
  from facts and rules together; a base view reads its predicate's base
  store.
* **`relic_fact_add()`.** A fact is visible to the next query without a
  check; adding it twice changes nothing; a fact that makes a negated
  literal false changes the answer of a query answered before it, under
  the default strategy and through the Magic rewrite; a fact the limit
  refuses changes nothing.
* **Limits over a session.** Queries with different binding patterns over a
  base store, run in turn under a memory limit, with loads between them and
  without, do not accumulate indexes; a load and a `relic_fact_add()` after
  materialization or a Magic query, under a tuple limit and under a memory
  limit, are not refused for the derived tuples; a load whose facts name
  several predicates and exceed the memory limit grows none of their
  stores.
* **Magic.** A bound query over a derived predicate with facts reads them
  through the base view, and `-e` shows one rule over it per adornment; its
  rows begin with its facts; seeds are inserted on every evaluation of a
  rewrite and shown as `facts: 1`; a rewrite abandoned over its budget leaves
  no warning about the predicates it generated; the budget goldens are
  re-derived.
* **Arity zero.** A fact of no columns is staged, stored, seeded and
  answered.
* **Counters.** Every hand-derived count in `tests/unit/test_eval.c` that
  includes a program's facts is re-derived, and the test helpers that sum
  the relations' bytes or tuples sum the base stores too.
* **Golden.** The language corpus gives the same answers under every
  strategy, and its goldens record this design's output: `-e` prints no
  component of facts alone and counts distinct facts, a derived
  predicate's rows begin with its facts, and warnings follow predicate
  identifier order. Every golden that differs from its predecessor is
  read, not regenerated blind.
* **Memory.** `facts-join` at scale 4 is measured, time and peak. What
  remains per fact at the peak is the data and the input: the symbols and
  values its constants intern, the tuple and its index slot, the text of
  the file, read whole, and the staging set live during the commit, about
  150 bytes for a ternary fact. The gate is below 200 bytes per fact.

---

## 10. Order of work

1. **Reservation for *n* tuples** in the relation store and the identifier
   table, with their tests.
2. **Base stores, derived predicates and the commit.** The shared rule
   append and the derived mark; the two-role funnel; `defined` and
   `referenced`; base stores and staging; the commit,
   fed by staging built from each fact statement the parser has already
   made, which the load then drops from its statements; seeding, the
   discard and the currency pair; base views, completeness and the budgets
   in Magic; explain's fact counts; `relic_fact_add()` as a load of one.
   Every counter test is re-derived. Gate: the language corpus and the
   Magic suites answer the same under every strategy, and the tests of
   section 9 but the memory gate pass.
3. **The parser's staging.** The scratch head and staging while parsing,
   so a fact never builds a statement. Gate: `facts-join` at scale 4 below
   200 bytes per fact at the peak, and its time measured against
   Souffle's.

Step 2 is one step because its parts are one change of meaning: facts
leaving the rule table is correct only once a derived predicate's
adornments read its base view and a rule's head cannot reach a base store.

---

## 11. Determinism

* A commit interns predicates, and appends each base store's tuples, in the
  order the load's text states them.
* Seeding inserts a base store in identifier order, before the first pass.
* The rules of a component run in program order after it.

So two runs over the same input produce the same identifiers, plans and
rows in the same order. The rows of a derived predicate with facts begin
with all its facts, materialized or under the rewrite, and the predicates
of a load's facts are interned before its rules' predicates.

---

## 12. Amendments this design makes

* Architecture section 5.1: facts are stored as the tuples of their
  predicate's base store, specified here. Sections 54 and 70: adding a fact
  commits it to its base store and discards what was derived.
* `specs/design-parser.md` section 5: the tree holds rules and queries,
  and the one fact it holds is the unsafe fact the check rejects; section
  6: the parser interns a fact's predicate, and a failed load leaves its
  fact predicates interned as it leaves its symbols and values; section 8:
  a load stages its facts and commits them before it splices.
* `specs/design-program.md` sections 3 and 11, and decision D-4 of
  `relic-plan.md`: the rule table holds the rules a program states; the
  shared append marks a head derived; `defined` as a function and
  `referenced` as a mark with its location, and the warning that asks
  both; and a load's fact predicates are interned as its facts are read,
  before the rules of earlier loads are compiled.
* `specs/design-relations.md`: base stores beside the relation table, the
  funnel's two roles, reservation for *n* tuples, and the refusal's
  activity.
* `specs/design-memory.md`: base stores in the account; the account bounds
  what the program's stores hold and what running the program does, and
  not the text being loaded or the load's staging; the mark that lets a
  discard with nothing derived return at once; and the identifier table's
  reservation for *n* entries.
* `specs/design-planning-cost.md`: the limits bound the program's facts
  as well as what running it does, so a load can be refused; and the
  program's facts do not raise the Magic budgets.
* `specs/design-eval.md` and `specs/design-evaluation-cost.md` sections
  3.4 and 7: seeding before the first pass, the currency pair, and facts
  stored as this document says.
* `specs/design-stratify.md`: the schedule lists components with rules,
  and a program's facts are in none of them.
* `specs/design-magic.md`: completeness is the derived mark, base views
  are interned before the rewrite, facts enter an adornment through the
  base view, the budgets' units, seeds as the rewrite's body-less rules,
  and the explain example.
* `specs/design-planner.md`: the explain examples that show fact counts.
* `specs/design-api.md` section 4: a programmatic fact is committed to its
  base store rather than appended as a statement.
* `include/relic.h`, `man/relic.1` and `man/relic.3`: the memory limit
  bounds the base stores, and a load or `relic_fact_add()` can be refused
  by a limit; `derivations` counts rules and seeding, not facts; a query
  over facts alone reads them without evaluating them; explain's fact
  count.
* In the code, every contract or comment that treats a fact as a rule,
  holds a predicate to one relation for the context's life, says only the
  evaluator inserts, treats every relation as re-derivable in full, or
  asserts that a discard leaves the store empty; among them
  `lib/account.h`'s statement of what the account bounds, which the
  design-memory amendment above restates.
* `STATUS.md` and `TODO.md` follow the implementation.

---

## 13. What is deliberately absent

* **Incremental insertion.** A fact added after materialization discards
  what was derived. A new base tuple is exactly a delta of an extensional
  predicate, so the base store is what incremental insertion needs, and it
  is architecture section 69's next step rather than this one.
* **A representation for imported facts.** CSV and TSV import is the same
  commit fed by a different reader, and needs no representation of its own
  (`specs/design-import.md`).
* **Retracting facts.** Nothing in relic retracts a fact, and a base store
  has no deletion.
* **Unwinding a refused commit.** Truncating the stores a refused commit
  had begun to fill would make consistency after a refusal the work of an
  unwind path, which the relation store avoids by ordering every fallible
  step first; measuring and reserving before appending keeps that ordering
  at the cost of one read-only lookup per staged fact.
* **Sharing a base store with its derived relation.** A derived predicate
  with facts could hold them once if its derived relation grew from its
  base store, but a discard would then have to cut derived tuples out of a
  store that must keep the facts.
* **A fact representation in the tree.** A statement kind or a block of
  facts would keep facts in the program's pipeline, and every evaluation
  would copy them from the program into the store; the base store keeps
  them where they are read.
