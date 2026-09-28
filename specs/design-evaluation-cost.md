# relic evaluation cost design

**Status.** Design document for what a derivation costs: how a semi-naive
round is bounded so that every derivation is made exactly once, how a delta
literal is placed and reached wherever the planner puts it, why the
identifier table keeps its four-byte slot and how insertion overlaps the
misses of its duplicate check instead, and why the fact-loading gap needs a
design of its own. Forward-looking, like every document under `specs/`;
`STATUS.md` records what is implemented.

**Companions.** `specs/design-eval.md` sections 2 and 4, whose delta variants
this document bounds; `specs/design-plan.md` sections 3 to 5, whose
operations gain a view; `specs/design-planner.md` sections 4 and 5, whose
estimate and order the delta literal now takes part in;
`specs/design-index.md` sections 3 and 4, whose chains gain a frontier;
`specs/design-memory.md` section 6, whose table section 5 measures;
`specs/design-relations.md` section 5 for the canonical index;
`specs/design-bench.md` for the comparison that found the gaps;
`relic-plan.md` decisions D-4, D-8, D-9 and D-10, the last of which this
document amends.

---

## 1. What this document settles

* the four measured causes of the gap to Souffle's Magic Sets build, and
  which change closes which;
* **views**: what each operation of a semi-naive variant may see of a member
  of its component, fixed when the round starts, so that a round's own
  insertions are invisible to it by construction and every derivation is
  made once;
* round 0 seeds the members with their facts and runs the body-less and exit
  rules only, and the first delta is the whole of each member;
* **the delta literal is placed by the planner like any other literal**, and
  a lookup reaches the start of a delta through a per-key **frontier** of the
  relation's own index, without copying the delta and without an index of
  its own;
* the estimate a delta gets, the one a member read beside it gets, and why
  a variant whose delta is not placed first keeps two plans and lets each
  round choose between them by the delta's size;
* **the identifier table keeps its four-byte slot**: a hash stored beside
  each identifier was measured, and on the closure workloads, where almost
  every insertion is a duplicate, it gives a mixed result in time for a
  larger peak;
* **batched insertion**, which overlaps the misses of the duplicate check
  that remain, and every view resolved when a plan starts, which makes
  when a batch is inserted invisible to every plan by construction;
* what is left of the fact-loading gap, and why it needs a design of its
  own;
* what a reader of `-e` and of the golden tests sees change, and the counts
  the tests hold.

It changes which tuples an operation reads and when a derived tuple is
inserted. It does not change what a plan is, what a relation holds, or any
answer: two runs of the same input still produce the same rows in the same
order, and
`-X naive` still answers every program the semi-naive evaluator does, which
is what keeps it the oracle of D-9.

---

## 2. The gap, measured

`bench/compare/run.sh` runs one set of workloads through relic and through
Souffle compiled with `--magic-transform`. Where Souffle is faster the
rewritten programs are identical, so the whole gap is in evaluation. Four
causes account for it, each measured with `-X stats` or a sampled profile;
the ratio is Souffle's median over relic's.

| Workload | Ratio | Cause |
| --- | --- | --- |
| `tc-cyclic-bf` | 0.21 | 1, 2 and 3 |
| `tc-acyclic-bf` | 0.35 | 1 |
| `chain-closure`, `sg-cyclic`, `tc-cyclic` | 0.64 to 0.79 | 3, and some of 2 |
| `graph-left`, `graph-reach`, `facts-join`, `tasks-ready` | 0.67 to 0.93 | 4 |

**Cause 1: the delta literal always drives the join.** A delta is a range of
tuple identifiers, per D-10, and only a scan walks a range, so every delta
variant places the delta literal first. For the rewritten bound closure

```prolog
tc$bf(X, Z) :- magic$tc$bf(X), par(X, Y), tc$bf(Y, Z).
```

the variant scans each delta tuple `(Y, Z)`, looks `par` up by `Y` for about
48 tuples, and tests each against `magic$tc$bf`, which holds 446 values and
rejects most of them. On the dense acyclic graph at scale 1 that is 6.27
million tuples looked up for 1.77 million derivations. Souffle's loop nest
is `for magic(X), for par(X, Y), for delta(Y, Z)` through an index on the
delta. The 446 magic nodes have 9,374 `par` edges between them, about 21
each, so the nest starts 9,374 lookups into the delta per round, some
112,000 over the 12 rounds, and visits one delta tuple per derivation:
1,109,055 combinations of a magic node, an edge and a delta tuple exist,
which is the number of derivations too. About 1.2 million tuples looked
up against 6.27 million, and 1.1 million derivations against 1.77.

**Cause 2: a round repeats work.** Round 0 runs every rule of the component
in full, recursive rules included, and a lookup takes the relation's end when
it starts, so a rule's later bindings see what its earlier ones derived. The
first delta is then everything round 0 inserted, and round 1 joins all of it
again. In later rounds a variant's non-delta literals read the whole
relation, the current round's insertions included, so one combination of
body tuples can be derived by two variants. On the complete graph on 40
nodes with loops, `tc(X, Z) :- par(X, Y), tc(Y, Z)` makes 132,800
derivations; the derivations that exist to be made number 68,800 (section
8). On `tc-cyclic-bf` it is 74.2 million against about 49 million.

**Cause 3: a derivation costs about three times Souffle's.** Profiles of
`tc-cyclic` and `sg-cyclic` put about three quarters of the time in the
duplicate check of `relic_relation_insert()`. The canonical index holds
tuple identifiers and nothing else, so every slot a probe visits costs a
read of the tuple it names, at a random address in a column array that does
not fit in cache once the relation holds a million tuples. The table is up
to three quarters full: with linear probing an unsuccessful search, which is
what inserting a new tuple is, visits on average between 1.8 slots just
after a doubling and 8.5 just before one, and every visited slot but the
empty one that ends the walk reads a tuple. But almost every insertion is
of a tuple the relation holds already: `tc-cyclic` makes 49.8 million
derivations for 2.05 million insertions, and `sg-cyclic` 25.5 million for
1.97 million, so 96 and 92 per cent of the duplicate checks are successful
searches, each a miss on the slot array and a miss on the tuple that
confirms the match. Souffle keeps the relation in a B-tree and runs its
outer loop over the head's first column, so consecutive insertions land in
one leaf.

**Cause 4: loading dominates the small workloads.** Loading is 0.07 of the
0.08 s of user time `graph-left` takes, and about half of `facts-join` at
scale 4. The symbol table recomputes every symbol's byte hash when it grows,
a fact is kept as a syntax tree statement and a compiled rule as well as a
tuple, and `facts-join` peaks at 136 to 153 MB of resident memory for
300,000 ternary facts, some 450 to 500 bytes per fact.

Sections 3 and 4 close causes 2 and 1, in that order, because bounded views
are what make placing the delta anywhere safe. Section 5 says why storing
the hash in the slot does not close cause 3, and section 6 overlaps the two
misses a duplicate costs. Section 7 says what is left of cause 4.

---

## 3. Views: what a variant may see

### 3.1 The four views

A member of a recursive component has, throughout the component's fixed
point, a **delta**: the half-open range of identifiers `[first, end)` of the
tuples it gained in the previous round, per D-10. That one range defines
three views of the member, and every operation over a relation reads through
exactly one of four:

| View | Range | Meaning |
| --- | --- | --- |
| `all` | `[1, relation end)`, taken when the plan starts | the relation as it stood when the plan started; the only view of a complete relation |
| `old` | `[1, first)` | the member's tuples older than its delta |
| `delta` | `[first, end)` | the tuples the member gained in the previous round |
| `current` | `[1, end)` | everything the member held when the round began |

The view is a property of the operation, set by the planner; the range it
denotes is resolved by the evaluation for every operation of a plan when
the plan starts, before it has inserted anything, from the member's delta
or, for `all`, from the relation's end.

```c
enum relic_view {
	RELIC_VIEW_ALL,		/* the relation as it stood when the plan started */
	RELIC_VIEW_OLD,		/* a member: tuples older than its delta */
	RELIC_VIEW_DELTA,	/* a member: its delta */
	RELIC_VIEW_CURRENT	/* a member: everything up to its delta's end */
};
```

`relic_op_atom` carries a `view` in place of its `delta` flag. `all` is the
view of every operation of a base plan, a query's plan and a naive
evaluator's plan, and of every operation over a complete relation
anywhere; the other three occur only in semi-naive delta variants, only over
members of the component being evaluated.

### 3.2 The views of a variant

Take a rule of a recursive component whose positive literals over members
of the component are, in body order, at body indexes *m1* < *m2* < ... <
*mk*. The rule has *k* variants, as `specs/design-eval.md` section 4.1 says,
and variant *i* reads:

* literal *mi* through the **delta** view;
* every member literal before it in body order, *m1* to *mi-1*, through the
  **old** view;
* every member literal after it, *mi+1* to *mk*, through the **current**
  view;
* every other literal through `all`.

The views are assigned by body order and not by plan order. The order the
planner chooses is the nesting of the join and decides what each binding
costs; the view decides which tuples a literal can match, and the set of
tuples a join produces does not depend on its nesting. The two questions are
therefore separate, and the planner is free to place the delta literal
anywhere (section 4).

A negated literal never reads a member, because that would be recursion
through negation and the program would have been rejected; a comparison
reads no relation. Neither has a view.

### 3.3 Each operation under a view

* A `SCAN` walks the view's range; a delta scan is a `SCAN` whose view is
  `delta`.
* A `LOOKUP` walks its key's chain from the first chain tuple in the view to
  the first at or past the view's end. For `all`, `old` and `current` the
  walk begins at the chain's head, and only the end differs. For
  `delta` it begins at the first chain tuple whose identifier is at least
  `first`, which section 4.2 finds without walking the chain from its head.
* A `MEMBER` asks the canonical index for the identifier of the tuple its
  terms name, and holds when that identifier lies in the view's range. The
  store's find returns the identifier rather than a boolean for this
  purpose; `relic_relation_contains()` is that function compared against
  `RELIC_ID_NONE`.
* `ANTI_MEMBER` and `FILTER` never read a member and are unchanged.
* `EMIT` inserts, through the batch of section 6. The tuple it inserts
  takes an identifier past the relation's end, which is at or past every
  view's end, so **nothing in the round can see it**. That is not a rule
  the executor obeys but a fact about the ranges: no view of a member
  reaches past `end`, and `end` was the member's end when the round
  began.

### 3.4 Round 0 and the deltas

**Round 0 seeds the members and runs the exit rules of the component.**
Each member whose base store holds facts is seeded from it first
(`specs/design-eval.md` section 3), and then the body-less rules -- a
rewrite's seeds -- and the exit rules, the rules with no positive literal
over a member, run each once, in program order. A recursive rule, one with
such a literal, has no base plan under the semi-naive fixed point and runs
only as its variants; its base plan exists under `-X naive`, where every
rule runs in full every pass.

At the start of the component every member's delta is set empty at the
first identifier, `[1, 1)`, and a member that already holds tuples is
recorded as having gained, as though round 0 had inserted them. The derived
relations were discarded before the evaluation began, since the program was
not current -- its rules or its base stores' facts had grown since the pair
of counts recorded at the last fixed point, or no pair was recorded -- and
nothing but evaluation writes a member, so what a member holds at that point
is exactly the facts seeding put there; the rule makes each of them part of
the first delta as a gain of round 0, and makes the argument of section 3.5
independent of what the member held, which a recursive rule that no longer
runs in round 0 would otherwise depend on silently. Round 0 then inserts the
body-less rules' tuples and what the exit rules derive. **The first delta of
each member is therefore the whole of it**, and round 1 is the first round to
run a variant.

**Invariant: at the start of every round, each member's delta is exactly
the tuples it gained in the previous round, possibly none, and its end is
the member's end.** It is kept at the round boundary by advancing, **once
each**, the members of the union of two sets: those that gained in the round
just run, and those whose delta was nonempty in it. Each is advanced to
`relic_relation_since(member, delta.end)`: for a member that gained, the
tuples since the old end; for one that did not, an empty range at its end.
Advancing a member twice would empty a delta it had just been given, so the
boundary walks the previous round's nonempty set first, skipping the
members marked as having gained, and then the gainers. A member in neither
set has an empty delta at its end already, and stays so. The cost of a
boundary is the size of the two sets, so a recursive cycle of *n*
predicates still takes *n* rounds of one member each, and the workload of
`tests/perf.sh` that holds that bound still holds it.

The evaluation keeps the member's delta and derives the three ranges from
it; it keeps nothing else per member per round.

### 3.5 Why every derivation is made exactly once

Number the rounds from zero and say a tuple of a member belongs to the round
that inserted it. Take a rule, and any way of satisfying its body: a tuple
for each positive literal, the member tuples being *t1* to *tk* in body
order, and let *m* be the latest round any of them belongs to.

Round *m* inserted a tuple, so round *m* + 1 runs. At its start every
member's delta is its round-*m* tuples, its `old` view its tuples of rounds
before *m*, and its `current` view its tuples of rounds up to *m*. Let *i*
be the first position whose tuple *ti* belongs to round *m*. In round *m* +
1, variant *i* finds *ti* in its delta view, every *tj* with *j* < *i* in
the old view, since those belong to rounds before *m*, and every *tj* with
*j* > *i* in the current view, since none belongs to a later round; the
complete literals are matched in `all`. So variant *i* derives the head from
exactly these tuples in round *m* + 1: **completeness**.

No other variant and no other round does. Variant *j* with *j* < *i* reads
*tj* through the delta view, and *tj* belongs to a round before *m*, not to
round *m*. Variant *j* with *j* > *i* reads *ti* through the old view, and
*ti* belongs to round *m*, not before it. A round before *m* + 1 has a
delta from a round before *m*, in which *ti* is not yet present in any
view. A round after *m* + 1 has a delta from a round after *m*, to which
none of the tuples belongs, so no variant finds its delta tuple among them:
**uniqueness**.

A combination whose tuples all belong to round 0, seeded facts and exit-rule
derivations, is the case *m* = 0: variant 1 derives it in round 1 from the
whole of each member, and no `old` view holds anything.

Termination is as before: every value a rule can produce was interned
before evaluation began, so each relation is finite, and a round that
inserts nothing is the last. The fixed point reached is the least one, for
the reason `specs/design-eval.md` section 3 gives.

The naive fixed point is untouched and remains the differential oracle: it
shares the executor, and under it every operation's view is `all`.

### 3.6 Which variants a round runs

A round runs, in planning order, the variants whose delta literal reads a
member that gained in the previous round, and among those only
the variants whose every `old` view is nonempty. A variant with an empty
`old` view derives nothing, since a join over an empty operand is empty, and
skipping it changes the cost and nothing else. It is what keeps round 1
from running the variants after the first of a rule with several member
literals against empty views.

An `old` view, once nonempty, stays nonempty, because a delta's start never
moves back. So each variant keeps a count of the `old` views it reads that
are still empty, set when the component is planned to the number of member
literals before its delta literal; when a member's delta first starts past
the first identifier, the count of every variant reading that member `old`
is decremented, once; and a variant is due when its count is zero. A member
crosses that point once, so the decrements over the whole fixed point
number the member literals of the component, and a round's cost stays the
variants it runs. Testing each due variant's `old` views at each round
instead would cost a rule of *k* member literals *k* squared comparisons a
round, which the 500-literal rule of `tests/perf.sh` would feel.

### 3.7 What changes

* **Rounds.** A round derives only from the previous round's tuples, so a
  component may take more rounds than an evaluator whose round 0 lookups
  see round 0's insertions and carry a chain several links in one round if
  the facts happened to be in its order. It never takes more than the
  longest derivation chain. `iterations` in `-X stats` changes accordingly.
* **Order.** Derivations happen in a different order, so the tuples of a
  recursive relation are inserted in a different order, and a query over one
  streams its rows in that order. The order is still a function of the
  source (section 10); the golden files of recursive programs are
  regenerated once.
* **Counts.** `derivations` falls to the number of ways the bodies can be
  satisfied, `tuples scanned` and `tuples looked up` with it. The unit tests
  that assert counters are re-derived by hand from this section.
* **Explain.** A recursive rule under the semi-naive fixed point prints its
  variants and no base plan (section 4.5).

---

## 4. Placing the delta

### 4.1 Why the delta was pinned first, and what it costs

D-10 pins the delta literal first in its variant "so that a delta is only
ever scanned, and no index over a delta is ever needed". The reasoning was
that a delta is the smallest input, and it usually is. It is not the
smallest input when a complete relation in the body is smaller still and
binds the columns the delta would be looked up by, which is exactly the shape
Magic Sets produce: a magic relation of a few hundred bindings guards a
rule whose delta holds the answers found so far for all of them. Section 2,
cause 1, is that shape, at a cost of five times the tuples looked up and
half as many derivations again.

Souffle's answer is to copy each delta into a relation of its own, with the
indexes the plans ask for. relic's is to leave the delta a range and make
the relation's own index able to start a chain walk at the delta's
beginning.

### 4.2 The frontier

A secondary index chains the tuples of a key in identifier order,
`specs/design-index.md` section 3, so the tuples of a key that lie in a
delta `[first, end)` are a contiguous run of the chain: the chain members
with identifiers at or past `first`, up to the first one at or past `end`.
A lookup stops at `end` by itself. What it cannot do unaided is *start* at
`first` without walking the chain from its head, which is what made the
delta
unplaceable anywhere but first.

Each key entry gains a third identifier, its **frontier**:

```c
struct relic_index_key {
	relic_tuple_id_t	first;
	relic_tuple_id_t	last;
	relic_tuple_id_t	frontier;	/* a chain tuple; every tuple before it is older than any delta still to come */
};
```

with the invariant that every chain tuple before the frontier has an
identifier below every delta start the index will be asked for. A new key's
frontier is its first tuple, which holds the invariant vacuously.

```c
relic_tuple_id_t relic_index_first_since(relic_index_t *, const relic_relation_t *,
    const relic_dom_t *probe, relic_tuple_id_t since);
```

finds the key as `relic_index_first()` does, then walks the chain forward
from the key's frontier while the identifier is below `since`, and returns
the tuple it reached: the first chain tuple in `[since, ...)`, or
`RELIC_ID_NONE` when the chain has none. The frontier becomes the tuple
reached, or, when the walk ran off the chain's end, **the chain's last
tuple**, which is still below `since` and so still satisfies the
invariant. A frontier is never `RELIC_ID_NONE`: the tuples a later round
links after `last` must remain reachable from it, and a walk from a lost
frontier would miss them without an error. The delta `LOOKUP` of section
3.3 calls it with the delta's `first` and stops, as every lookup does, at
the view's end. It takes a mutable index, because it moves the frontier.

A frontier step reads the `next` array and no tuple, and is not counted as
a tuple looked up; the counter of `specs/design-eval.md` section 4.3 keeps
counting the chain tuples a lookup reads and matches.

**It is amortized constant time because deltas advance.** Within one fixed
point a member's delta starts where the previous one ended, so the `since`
an index is asked for never decreases. The frontier of a key therefore only
moves forward, passes each chain tuple at most once over the whole fixed
point, and after the first call of a round every later call of that round
for the same key walks nothing. The total walked per key over the fixed
point is bounded by the chain's length, so the positioning costs at most
one pass over the relation, however many rounds and lookups there are.

**It is correct whatever it is asked.** The invariant depends on `since`
never decreasing, and the evaluator guarantees that: an index is dropped
with the discard that precedes every evaluation and every rewrite, an index
over a member is built at component start over an empty member, and a
relation is a member of one component. It is made unconditional rather
than trusted: the index records the largest `since` it has served, cleared
when the index is emptied, and a call with a smaller one walks that chain
from its head and leaves the frontier where it is. That call costs the
chain's length and happens on no path the evaluator has; it is a guard, and
it has no cliff.

**The order of a lookup is unchanged.** The walk is forward along the
chain, so a delta lookup visits exactly the tuples a delta scan matching the
key columns would, in the same order, which is what D-4 asks of every
generator and what keeps derivation order a function of the source.

**The cost is four bytes per distinct key** of each secondary index,
charged to `max_memory` through `relic_index_bytes()` as the entries are
already. Nothing is copied, no relation is created, no index is built or
rebuilt per round, and a round still allocates nothing.

Building an index over tuples a relation already holds sets each key's
frontier as insertion would have; an index over a member is built at
component start over an empty member in any case.

### 4.3 The planner

The delta literal is **ordered with the other positive literals** by the
greedy order of `specs/design-planner.md` section 5.1, scored like any
other, and becomes the operation its bound columns give it at the point it
is placed, per section 5.2 of that document. The planner learns each
literal's view from what it already has: the delta literal's index, the set
of incomplete predicates and body order, and the static mode of the
rewrite, which asks for no delta literal, is untouched. The operation is:

* `SCAN` through the delta view when no column is bound;
* `LOOKUP` through the delta view, on the index keyed on its bound columns,
  when some are, positioned by section 4.2;
* `MEMBER` through the delta view when every column is bound.

The other member literals of the variant are placed by the same order and read
their `old` or `current` view. Under `-X no-index` every lookup becomes a
scan, so a delta placed after other literals would be scanned whole once for
every binding that reaches it; there the delta literal is placed first, as a
`SCAN` of the delta view, and the variant has that one plan. Under `-X
no-reorder` positive literals are placed in the order they are written, the
delta literal in its written place, so the textual reference is the textbook
semi-naive scheme and nothing more.

**A variant has two plans when the order places the delta anywhere but
first**, and one otherwise. The **placed plan** is the order above. The
**delta-first plan** is the same walk with the delta literal pinned first.
Each records one estimate, the product of the
estimates the positive literals on one side of the delta literal were
placed by:

* the placed plan's **prefix estimate**, over the literals before the
  delta: the planner's figure for the bindings that reach the delta literal
  in a round, each costing one lookup of the delta, whatever the delta
  holds;
* the delta-first plan's **fan-out estimate**, over the literals after it:
  the planner's figure for the bindings each delta tuple leads to.

A round whose delta holds *D* tuples then costs the placed plan about the
prefix and the delta-first plan about *D* times the fan-out, and the
**threshold** of the variant is the prefix over the fan-out, in integer
division. When the order places the delta first the two plans coincide, the
prefix is empty, and only one plan is built. Two degenerate estimates
settle the choice for every round: a prefix of zero, a relation before the
delta found empty, makes the placed plan read nothing and the threshold
zero; otherwise a fan-out of zero makes the delta-first plan the cheaper
whatever the delta holds, and the threshold is one no delta exceeds.

**Both orders are decided before either plan is emitted**, and only a plan
some delta would select is emitted: a round runs only for a delta of at
least one tuple, so a threshold of zero leaves the delta-first plan
unbuilt, and one no delta exceeds leaves the placed plan unbuilt. Since
emitting a plan is what creates the indexes it reads, a plan no round
would run costs no index kept up through the fixed point.

**Each round chooses**: a variant runs its placed plan when its delta holds
more tuples than the threshold, and its delta-first plan otherwise. The
delta's size is known when the round starts, the comparison is one integer
against another, and nothing is planned or allocated in the round. Section
4.4 says why the choice cannot be made once for all rounds, and why both
sides of it carry an estimate.

The static mode the Magic rewrite plans in, `specs/design-planner.md`
section 7, asks for the order of a rule with no delta literal and is
untouched.

### 4.4 The estimate

The planner cannot measure a member, `specs/design-planner.md` section 3,
and guesses `RELIC_PLAN_UNKNOWN_TUPLES` for an incomplete relation, 1000,
narrowed by `RELIC_PLAN_UNKNOWN_DISTINCT`, 10, per bound column. The guess
now depends on the view:

* a member read through `all` or `delta` estimates `RELIC_PLAN_UNKNOWN_TUPLES`;
* a member read through `old` or `current` estimates
  `RELIC_PLAN_UNKNOWN_TUPLES` x `RELIC_PLAN_DELTAS_HELD`, with
  `RELIC_PLAN_DELTAS_HELD` 10: a member read beside its delta is taken to
  hold ten deltas.

The second rule says only that a member is larger than its own delta, which
is true by construction, and ranks the delta ahead of every other member
literal with the same bound columns, so the variants of a rule over its own
component alone put the delta first unless a constant binds a column of
another member literal, when that literal's bound argument ranks it first,
as the greedy order ranks every literal. The first rule leaves `all`, which
competes with a delta in no plan, where it was, so the base plans of `-X
naive` and the rewrite's static mode are unchanged.

What the constant decides is which complete relations the placed plan puts
before the delta: those the estimate puts below 1000 tuples, and, since the
greedy order ranks bound arguments before estimates, every literal that a
placed literal binds a column of. Three bodies show the consequences, with
`par` holding 50,000 tuples and `magic` 446:

```prolog
tc$bf(X, Z) :- magic(X), par(X, Y), tc$bf(Y, Z).
```

Nothing is bound at first. `magic` estimates 446, the delta of `tc$bf`
1000, `par` 50,000, so `magic` is placed and binds `X`; `par` then has a
bound column and is looked up on `X`; the delta is last, looked up on `Y`
through the frontier. That is Souffle's loop nest. The prefix estimate is
446 times `par`'s estimate for one bound column, 50,000 over its 1000
distinct first columns, about 22,300. Pinned first, the delta is scanned,
`par` looked up on `Y`, about 50 tuples a binding, and `magic` tested, one:
a fan-out of about 50, and a threshold of about 446. A round whose delta
holds more runs the placed plan, a smaller one the delta-first plan, and
the tuples looked up fall from 6.27 million to about 1.2 million
(section 2).

```prolog
tc(X, Z) :- par(X, Y), tc(Y, Z).
```

The delta estimates 1000 against 50,000, is placed first and scanned, and
`par` is looked up on `Y`: the delta-first order, and the only plan.

```prolog
p(X, Z) :- p(X, Y), p(Y, Z).
```

Variant 2 reads the first `p` old and the second delta: 10,000 against
1000, so the delta is scanned and the old `p` looked up on `Y`.
Variant 1 reads the first delta and the second current, 1000 against
10,000, the same way round. One plan each.

**Why the round chooses.** The planner cannot know a delta's size, and no
constant stands in for it. Take `r(X, Z) :- g(X), big(X, Y), r(Y, Z)` with
`g` holding 999 tuples and `big` a thousand `Y` per `X`, each `Y` reached
from ten `X`. The placed plan loops over the 999,000 bindings of `g` and
`big` every round and looks each up in the delta; the delta-first plan
costs each round its delta times `big`'s fan-in, ten. On a round whose
delta holds one tuple the placed plan is a hundred thousand times the
work, and on one whose delta holds a million it is a tenth of what the
delta-first plan would do. Neither plan is right for both rounds, and a
fixed point has both kinds.

**Why both sides carry an estimate.** Comparing the delta's size with the
prefix alone would weigh the placed plan's outer loop against the
delta-first plan's outer loop and ignore what each delta tuple goes on to
cost, which is the whole of that plan's work when the literals after the
delta fan out: under it `tc$bf` above would run the delta-first plan on
every round whose delta holds fewer than 22,300 tuples, and each of those
tuples looks up about 50. Weighing the prefix against the delta times the
fan-out compares the two plans' work, each side up to the accuracy of the
planner's estimates, and on the `r`, `g` and `big` body runs the placed plan
once a round's delta exceeds 99,900 tuples.

The comparison is between the parts of the two plans that differ. The work
after the delta literal in the placed plan, the delta tuples it matches and
what they join with, is work the delta-first plan does as well, and is left
out of both sides, and so is the delta-first plan's own scan of the delta.
A positive literal the delta-first plan tests for membership contributes
its estimate to the fan-out, one tuple unless its relation is empty, so a
guard that rejects most bindings makes the delta-first plan look dearer
than it is and moves the threshold down; the placed plan is then chosen a
little early, which costs at most the prefix's lookups. A relation found
empty after the delta makes the fan-out zero and the placed plan never
run, even for a prefix of one binding; such a rule derives nothing either
way.

The choice is deterministic, because the sizes are; it is bounded, because
it is a comparison; and it is explainable, because both plans and the
threshold are printed once (section 4.5) and the choice follows from the
counters. What it is not is replanning: the plans are built when the
component is, as `specs/design-planner.md` section 2 requires, and the
indexes they name with them.

### 4.5 Explain

An operation over a member of the component prints its view after its kind,
for the views a reader needs to see: `scan delta`, `lookup delta`, `member
delta`, `scan old`, `lookup old`, `member old`. The `current` view is
unmarked, since it is what a member literal in a variant reads unless
marked otherwise, and `all` is unmarked. For the graph program of
`specs/design-planner.md` section 8.2:

```text
stratum 0, component 1 (recursive): path/2
    rule path(X, Y) :- edge(X, Y).
        scan edge(X, Y)
        emit path(X, Y)
    rule path(X, Z) :- edge(X, Y), path(Y, Z).
        delta path(Y, Z):
            scan delta path(Y, Z)
            lookup edge(X, Y) key={1}
            emit path(X, Z)
```

A recursive rule shows its variants and no base plan, because the lines are
what ran (`specs/design-planner.md` section 8.1) and no base plan runs.
Under `-X naive` it shows its base plan and no variants. A
variant with two plans shows both, the placed plan headed by its threshold
and the delta-first plan after it:

```text
        delta tc$bf(Y, Z) when the delta holds more than 446 tuples:
            scan magic$tc$bf(X)
            lookup par(X, Y) key={0}
            lookup delta tc$bf(Y, Z) key={0}
            emit tc$bf(X, Z)
        delta tc$bf(Y, Z) otherwise:
            scan delta tc$bf(Y, Z)
            lookup par(X, Y) key={1}
            member magic$tc$bf(X)
            emit tc$bf(X, Z)
```

A variant left with one plan by its threshold shows that plan alone,
under the heading of a variant with one plan, and a threshold of one says
`tuple`.

The lines are still what ran, or could have: which plan each round chose
follows from the counters and is not printed, so that `-e` output stays a
function of the plans and not of how many rounds ran.

The component's plans are recorded per rule, each rule with its base plan
or its variants, which is what lets explain pair them without walking two
arrays in step, and what counts a component's body-less rules, which its
`facts:` line adds to the facts its members' base stores hold: the rules
with no plan, rather than the difference of two array lengths, which would
count every recursive rule as body-less once it has no base plan.

---

## 5. The identifier table keeps its slot

### 5.1 What a probe costs

`specs/design-memory.md` section 6 gives the identifier table one design
rule: a slot holds an identifier and nothing else. The key stays with its
owner, so the table costs four bytes per slot and a probe that finds a
candidate asks the owner whether the candidate's key is the one wanted. For
the canonical index of a relation that question reads the candidate tuple's
columns, at an address the identifier determines and the probe does not
predict. Every visited slot but the empty one that ends an unsuccessful
search is therefore a dependent read of a random line of the column array,
the matching slot of a successful one included, and with the relation past
the cache that is a miss each.

The same shape costs the secondary indexes twice over: a key's table holds
key identifiers, and comparing a candidate key reads the first tuple of its
chain, `specs/design-index.md` section 3, through the key's entry -- two
dependent reads per visited slot. And it costs every growth an indirect
call per entry into the owner to recompute the hash from the key, which for
the symbol table means rehashing every symbol's bytes and for a relation
means rereading every tuple.

### 5.2 A hash beside each identifier, measured

The alternative is a slot of eight bytes, the entry's hash beside its
identifier. A probe would compare the stored hash first and ask the owner
only about an entry whose hash matched, so an unsuccessful search would
read no tuple at all, and a growth would place each entry by its stored
hash with no callback into the owner, deleting the eight rehash functions
of the symbol, value and predicate tables, the canonical and secondary
indexes, the distinct count and the rewrite's two tables.

Measured against the four-byte slot, with neither batching, on the closure
workloads whose profile section 2 cause 3 describes and on `facts-join`;
times are the median user time of five interleaved runs, peaks the range
of the maximum resident set over them, and the share is of a sampled
profile:

| Workload | Time, 4 / 8 bytes | Peak, 4 / 8 bytes | Duplicate check, 4 / 8 bytes |
| --- | --- | --- | --- |
| `tc-cyclic` | 4.75 / 4.36 s | 81-82 / 90-116 MB | 73 / 78 % |
| `sg-cyclic` | 3.42 / 3.56 s | 56-69 / 77-102 MB | 68 / 72 % |
| `facts-join` | 0.35 / 0.27 s | 136-153 / 147-163 MB | |

The stored hash saves the tuple reads of the candidates that are not the
match, and these workloads make few: section 2 counts 92 to 96 per cent of
their duplicate checks as successful searches, which at loads between three
eighths and three quarters visit 1.3 to 2.5 slots on average and read the
matching tuple with or without the stored hash. What the stored hash does
change is the slot array, which doubles; the likely reading of the table is
that a larger array misses more, and that the time the saved tuple reads
gave back went to slot reads. What it gains is the growth without a
callback, about a quarter of `facts-join`'s time.

**The slot stays four bytes.** The closure workloads pay the two dependent
misses of a successful search, one it would not remove, for a mixed result in
time and a larger peak on both, and section 6 overlaps those misses rather
than removing one. The stored hash is worth its slot again for a workload
whose insertions are mostly new tuples, whose unsuccessful searches read a
tuple per visited slot; the measurement that would show it is the profile
share of the duplicate check against the peak, taken with section 6 in place.

**A tag is the cheaper form this section did not measure.** One byte per
slot holding the top eight bits of the hash lets an unsuccessful search pass
most slots whose identifier is not its key without the doubled slot array,
and `specs/design-comparison-cost.md` section 6.2 gives one to the tables
whose searches mostly miss. Measured on the canonical index, it made the
closures faster and their peaks higher, as the stored hash did, so the
finding above stands for that index and it stays untagged.

---

## 6. Batched insertion

### 6.1 What it overlaps

A duplicate check that finds its tuple, which section 2 counts as nearly
all of them, costs two dependent misses: the slot the tuple's hash selects,
and the tuple that slot names, read to confirm the match. The executor
makes one check per derivation and waits out both misses before it derives
the next tuple, so the misses of consecutive derivations are paid one after
another although nothing orders them. Batching lets them overlap: gather
several derived tuples, start the slot reads of all of them, then the reads
of the tuples their first slots name, and only then insert each in turn,
finding the lines already on their way. A search that visits more slots
waits out the later ones as before.

### 6.2 Why it is sound

An `EMIT` appends its head tuple to a buffer, and the executor hands the
buffer back when it fills and when the plan ends. Its caller inserts it,
or, in a round whose insertions wait for its end (`specs/design-eval.md`
section 4), screens it against the store and gathers the tuples the store
lacks, `specs/design-parallel.md` section 5.3; either way it takes each
batch before it asks the executor for the next, so no batch is lost and
none outlives its plan.

A batch inserted while its plan runs is invisible to that plan by
construction, not by an argument about which plans read what. Every view
of a plan is resolved when the plan starts, section 3.1, before it has
inserted anything, so every view ends at or before its relation's end at
that moment: a member view at the delta's end, which is the member's end
when the round began, section 3.3, and `all` at the relation's end. A
batch takes identifiers past it. So what a plan reads does not depend on
when, or in how many batches, its tuples are inserted, and
`RELIC_INSERT_BATCH` is a constant of performance alone.

Resolving `all` at the plan's start is what the naive fixed point and a
one-pass component see: a rule sees what the rules before it inserted, as
`specs/design-eval.md` section 3 promises, and none of its own derivations
of the same pass. That changes how many passes the naive fixed point takes
and not where it ends, as that section already says of the order rules run
in; a pass still ends the fixed point only when it inserted nothing. The
rows of a query's answer relation are read only after its plan ends.

The buffer is inserted in the order its tuples were derived, one at a time,
so every tuple takes the identifier it would have taken inserted alone at
the moment its batch is inserted, and a tuple equal to an earlier one of
the same batch is found as a duplicate of it. When the evaluation succeeds,
`derivations` and `tuples inserted` total what unbatched insertion totals.
A refusal inserts the tuples before the refused one and discards the rest,
which the failed evaluation discards anyway. The tuple limit refuses the
same tuple with the same diagnostic, but reports it when the batch is
inserted, so the executor may have run past it: up to a batch less one of
further derivations, any number of tuples scanned and looked up on the way
to them, and a mixed-comparison note among those bindings, all of which
the counters and the diagnostics record. The buffer is charged to the
evaluation arena, so a memory limit can fall a buffer's size earlier.

### 6.3 The insertion

`relic_relation_insert_batch()` takes a relation, its tuples packed at its
arity, and a count, and reports for each whether it was added and how many
insertions completed; on a refusal those before the refused one completed,
and it and the rest are reported not added. The tuples pointer is never
null, even for a relation of arity zero, whose tuples occupy none of it. It
works in chunks of `RELIC_INSERT_BATCH` tuples, so no caller can hand it
more than its scratch holds, in three passes over a chunk:

1. hash each tuple and prefetch the slot its hash selects;
2. read each tuple's first slot, now arriving, and prefetch the tuple it
   names, if any;
3. insert each tuple in order by the hash pass 1 computed, as a single
   insertion would.

The table's own helpers do the first two passes, and they touch no slot of
a table with no capacity, which is what a relation's first insertion
finds. Pass 3 may grow the canonical index, after which the lines
prefetched for the old slot array are simply not used: every address was
computed from live pointers before the growth, a prefetch is a hint that
reads nothing a later pass depends on, and it cannot fault. The insertion
of a single tuple is the same code as pass 3 with its hash computed there,
so the two cannot disagree about what a duplicate is.

The prefetch is `RELIC_PREFETCH(address)`, the compiler's builtin where
`configure` finds one and nothing elsewhere, where batching still inserts
correctly and only loses the overlap. A builtin is a development-tool
question and not a dependency in the sense of architecture section 74.

### 6.4 The buffer

One buffer per evaluation for the plans it runs itself, since one of them
runs at a time and the buffer is empty between plans, and one per worker
for a round whose insertions wait for its end, which belongs to the worker
pool (`specs/design-parallel.md` section 5.3). The evaluation's is
`RELIC_INSERT_BATCH` tuples of the widest head among the rules the
evaluation is given, or of the query's answer, a head of arity zero
counting as one column so that the buffer always exists. It is allocated
from the evaluation arena once, when the evaluation begins, and charged as
the arena is, so a round the evaluation runs itself allocates nothing, a
program of narrow relations pays for narrow tuples, and a program of many
rules costs no more than one of few. A plan has one `EMIT`, so the buffer
holds tuples of one relation until it is inserted. `RELIC_INSERT_BATCH`
is 64: measured on `tc-cyclic` and `sg-cyclic`, 16 tuples overlap most of
the misses, 64 a little more, and 256 no more than 64.

---

## 7. Loading facts

An identifier table doubles by asking its owner for the hash of every
entry, which rehashes every symbol's bytes at each doubling of the symbol
table and rereads every tuple at each doubling of a relation's canonical
index. Section 5.2 measured what storing the hash in the slot would save,
about a quarter of `facts-join`, and left it for its memory.

The larger cost is that of a fact held as a program object. A fact is a rule
with an empty body, `specs/design-parser.md` section 5, and a representation
that followed the language would keep its statement in the syntax tree,
which is append-only and never discarded, compile it into a `relic_rule_t`
with its head arguments in the program arena and an entry in the rule
table, record it in its component's schedule, and have evaluation insert its
tuple. Per ternary fact that is a statement with three terms and their
locations, a compiled rule with three arguments, and a schedule index:
several hundred bytes, against the twelve of the tuple and the handful of
its index slot. Measured on that representation, it was the 450 to 500
bytes per fact of `facts-join`'s peak in section 2, and the loading time of
the small workloads there was the parsing, checking and scheduling of each
fact as a rule, before its tuple was ever inserted.

Note that a symbol is not interned twice in the sense of hashing its bytes
twice: the lexer interns the bytes into the symbol table, and the value
table hashes the symbol's identifier, one word, to intern the value. The
second is cheap; the first, and the rehash at each growth, are the
interning costs.

The remedy is a representation question, and `specs/design-facts.md`
settles it. A fact stays a rule with an empty body in the language, and is
stored as a tuple of its predicate's base store, costing a tuple and an
index slot and nothing per fact in the syntax tree, the rule table or the
schedule. A load stages its facts and commits them to the base stores; a
derived predicate's relation is seeded from its base store before its
component's first pass, section 3.4; the relations are current while
neither the program's rule count nor its base stores' tuple total has grown
since the last fixed point; and the Magic rewrite reads a derived
predicate's facts through one rule over its base view per adornment rather
than a guarded copy of each fact. The representation is that document's
subject and outside this one's.

---

## 8. The tests

Every change above is a change to counts an existing counter measures, so
the tests are counters compared exactly, plus the equivalences that say the
answers did not move.

**Unit tests, `tests/unit/test_eval.c`.** The scenarios that assert
`iterations`, `derivations`, `tuples scanned` and `tuples looked up` are
re-derived from section 3 and asserted exactly. Two are added:

* **The complete graph.** `par` holds every ordered pair of 40 nodes, loops
  included, 1600 tuples, and `tc(X, Y) :- par(X, Y). tc(X, Z) :- par(X, Y),
  tc(Y, Z).` is queried whole. No rule derives `par`, so its facts are read
  from its base store and seed nothing. Round 0 derives 1600 from the exit
  rule; round 1 joins the 1600 delta tuples with 40 `par` tuples each,
  64,000 derivations, none new, and the fixed point is reached. With the
  query's 1600 rows the derivations number 67,200 and the tuples looked up
  64,000. An evaluator whose round 0 runs the recursive rule and whose
  views reach the round's own insertions makes 131,200 and 128,000.
* **Two member literals.** `p(X, Y) :- edge(X, Y). p(X, Z) :- p(X, Y),
  p(Y, Z).` over a chain of *n* nodes. A body of the recursive rule is
  satisfied once per triple of nodes in chain order, so its variants make
  *n* choose 3 derivations over the fixed point, to which the exit rule's
  *n* - 1 add, the *n* - 1 facts of `edge` being read from its base store;
  the query adds *n* choose 2. For *n* = 20 that is 1140 + 19 + 190 = 1349
  derivations, which an evaluator
  whose views reach the current round's insertions exceeds. The test also
  covers the boundary of section 3.4 with two members, one gaining in a
  round the other does not.

`tests/unit/test_index.c` gains the frontier: a chain looked up from
successive marks visits each tuple once; a chain whose every tuple is below
the mark, then extended, then asked from a higher mark, finds the new
tuples; and a mark below the last stays correct.
`tests/unit/test_relation.c` gains batched insertion, section 6: batches of
sizes on both sides of `RELIC_INSERT_BATCH` insert what single insertions
would, under the same identifiers, and a limit refuses a batch where it
refuses single insertions. `tests/unit/test_eval.c` gains a plan under the
naive fixed point that derives more than a batch, so that a batch is
inserted under an open `SCAN` and before a `MEMBER` of the head, and whose
counters show the plan read none of it.

**Equivalences, `tests/equiv/strategies.sh`.** The language corpus under
`-X naive`, `-X no-index` and `-X no-reorder` gives the default's answers,
as today; the default's rows are in a different order for recursive
programs, and the golden files are regenerated once, with the diff read.

**Differential fuzzing**, `specs/design-hardening.md` section 3, compares
the semi-naive and naive fixed points on generated programs and is the
test that no view loses a derivation on bodies nobody wrote by hand. It
cannot see a derivation made twice, since a duplicate changes no answer, so
`tests/random` gains a count: for a program whose rules hold no `_`,
`derivations` must equal the distinct facts of the predicates rules derive,
which seed their relations, plus, for each rule, the number of distinct
bindings of its body variables over the final relations, which the driver
computes from the naive answer.

**Sanitizers.** `make check-asan` and `make check-ubsan` before each commit:
the frontier, the batch buffer and the per-rule plan records are memory
layout changes.

**Bounds.** `tests/perf.sh` holds its bounds unchanged: the reachability
counters, the linear cycle of 200,000 predicates, which section 3.4 keeps
linear, and the interactive session.

**Measurements.** `bench/run.sh` before and after each section, comparing
the counters exactly and the seconds by order of magnitude; and
`bench/compare/run.sh -w tc-acyclic-bf,tc-cyclic-bf,tc-cyclic,sg-cyclic,chain-closure`
against Souffle, whose ratios section 2 records and section 9 expects to
move.

---

## 9. Order of work

Each step is committed on its own, behind its measurement, and each is
useful without the ones after it.

1. **Views** (section 3). Gate: the complete-graph test passes; every
   counter test is re-derived and passes; `tc-cyclic-bf` derivations fall
   from 74.2 million toward 49 million; `bench/run.sh` shows no counter
   higher than before on any workload.
2. **The frontier and the delta's placement** (section 4). Gate:
   `tc-acyclic-bf` tuples looked up fall from 6.27 million to about 1.2
   million and its derivations from 1.77 million to about 1.1 million, its
   time toward Souffle's; no `bench/run.sh` counter rises; `-e` shows the
   orders section 4.4 works through.
3. **The identifier table** (section 5). Measured with the hash stored in
   each slot: a mixed result in time on the closure workloads and a larger
   peak on both. The slot stays; the measurement is recorded in section 5.2.
4. **Batched insertion and the `all` view fixed at the plan's start**
   (section 6), since the duplicate check still holds two thirds to three
   quarters of `tc-cyclic`'s and `sg-cyclic`'s profile. Gate: their times
   fall and the share of the duplicate check with them; no `bench/run.sh`
   counter rises but those of the naive fixed point, whose counters are
   re-derived by hand; the answers of `-X naive` are unchanged.
5. **Facts** (section 7): a profile of `facts-join` and `graph-left`, then
   `specs/design-facts.md`.

Step 1 goes first because step 2 is only safe once a round cannot see its
own insertions, and step 4 for the same reason.

---

## 10. Determinism

* A view's range is a function of the member's delta, which is a function
  of the rounds before it, which are a function of the source.
* The delta literal is scored by the same integer estimate as every other
  literal, with two constants in the source, and ties break by source
  position.
* A delta lookup visits a chain forward, in identifier order, from a
  position that depends only on the delta's start.
* A batch is inserted in the order its tuples were derived, and no plan
  reads its own batches, so the tuples every plan reads, and the
  identifiers its tuples take, are the same whatever `RELIC_INSERT_BATCH`
  is.

So two runs over the same input produce the same plans, the same derivation
order, the same rows in the same order and byte-identical `-e` output. Each
step of section 9 that changes the order changes it once, at its commit:
steps 1 and 2 for the semi-naive evaluator, and step 4 for `-X naive`.

---

## 11. Amendments this design makes

On acceptance the durable record changes in these places, and nowhere else:

* `relic-plan.md` D-10: the delta remains a range of tuple identifiers; the
  sentence pinning it first is replaced by section 4.
* `specs/design-eval.md` sections 2.1 and 2.2, where an `EMIT` inserts
  through the batch and the `all` view is fixed when the plan starts;
  sections 3, 4, 4.1, 4.2 and 4.3: a naive pass does not see a rule's own
  derivations of the same pass; the
  pseudocode, the variants and the completeness argument become sections
  3.2 to 3.5 here, the "refinement" 4.1 defers being what section 3
  specifies and the measurement it asked for being section 2; and `tuples
  looked up` says that a frontier step is not one.
* `specs/design-plan.md` sections 3 to 5.3: `view` replaces `delta` on an
  atom, `all` ends where the relation ended when the plan started, and 5.3
  no longer pins the delta first.
* `specs/design-relations.md` section 6: the batch insertion beside the
  single one, and the one hashed insertion both perform.
* `specs/design-planner.md` sections 5.1, 5.2, 6, 8.2 and 10: the delta
  literal takes part in the order; a variant may have two plans, with a
  prefix and a fan-out estimate and the threshold between them; the
  estimate depends on the view; `-X no-reorder` places
  the delta in its written place; the explain example and the `scan delta`
  sentence; "indexes over a delta" leaves the absent list, and "replanning
  during a fixed point" stays on it.
* `specs/design-planning-cost.md` section 4, step 1, which places the delta
  literal first per D-10, and section 5, whose bound gains a second plan
  per variant.
* `specs/design-magic.md` section 10: the explain example, whose recursive
  rules show base plans and whose small `edge` would now precede the delta.
* `specs/design-index.md` sections 3, 4, 8 and 10: the frontier, the
  positioned lookup, twelve bytes of `entries` per key, and the removal of
  "indexes over a delta".
* `specs/design-memory.md` section 6: the stored hash measured and not
  adopted, section 5.2, as a reason the slot holds an identifier alone.
* `specs/architecture.md` section 28 points here.
* `STATUS.md` and `TODO.md` follow the implementation.

---

## 12. What is deliberately absent

* **Replanning a variant each round from the delta's size.** The size is
  known when a round starts and would pick the order exactly; but a plan
  per round costs the planner's work per round, needs an index that may
  not exist yet built inside the fixed point, and makes `-e` depend on how
  many rounds ran, which `specs/design-plan.md` section 6.1 and
  `specs/design-planner.md` section 10 already refuse. Section 4.3 builds
  the two plans a round can choose between when the component is planned,
  and section 4.4 says why one of them is not enough.
* **A single static plan per variant.** A constant for the delta's size
  would place it once for all rounds, and section 4.4 shows a body for
  which every fixed choice is a million times wrong on some round.
* **A scratch relation per delta**, Souffle's design. It copies every delta
  tuple once, builds and drops the scratch's indexes every round, and
  points the delta literal's atom at a relation that changes identity
  each round; the frontier reaches the same tuples in the same order for
  four bytes per key and no copy.
* **A B-tree store.** The gaps of section 2 are a join order, repeated
  work and dependent reads, not a data structure; the hash store keeps
  membership constant-time and the tuple identifier a position, which
  D-10 rests on.
* **A hash stored in each slot.** Section 5.2: on duplicate-heavy
  workloads it trades a larger peak for a mixed result in time.
* **A lower load factor.** It shortens the unsuccessful searches, which
  the duplicate check rarely makes, and costs slots.
* **A different guess for the rewrite's static mode.** Section 4.4 leaves
  the estimate of an incomplete relation read whole where it is, so the
  sideways information passing of `specs/design-magic.md` section 5
  chooses its orders by the estimates that section describes.
* **The fact representation.** Section 7.
