# relic evaluation design

**Status.** Design document for evaluation: the executor that runs a plan,
the fixed points built on it, what a materialized program is and when it is
current, and how a query's answers reach a caller. Forward-looking, like every
document under `specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 15, 25 to 31, 39, 52, 53, 61
and 63; `specs/design-plan.md` for the plans this runs, and
`specs/design-planner.md` for when they are built;
`specs/design-relations.md` and `specs/design-index.md` for the store it
reads and writes;
`relic-plan.md` decisions D-6, D-7, D-8, D-9, D-10 and D-11.

---

## 1. What this document settles

* how a plan is executed without recursion, and what state that needs;
* what a comparison means when its operands have different types;
* the naive fixed point, why it terminates, and why it stays in the library;
* the semi-naive fixed point, why it is complete, and the invariant it rests
  on;
* the counters evaluation keeps, and what they are for;
* when the relations of a context hold the program's fixed point, and what a
  later load does to that;
* how a query is answered and its rows streamed, and why no row is delivered
  before the answer is complete;
* what a failure leaves behind.

---

## 2. The executor

The executor runs one plan: an array of operations ending in `EMIT`, as
`specs/design-plan.md` defines it. The join the plan describes is a nest of
loops, one per generator, and the executor runs that nest with **one loop and
an explicit level** rather than with a C call per level:

```text
level = 0, resume = false
forever:
	ask operation `level` for a match
	    (a fresh start if not resume, else its next match)
	matched:	level = level + 1, resume = false
	exhausted:	if level == 0, stop
			level = level - 1, resume = true
```

Descending means the operation at `level` produced a binding and the next one
should start fresh against it. Backtracking means an operation has nothing
more, so the one before it is asked for its next match. `EMIT` never reports a
match -- it inserts and says there is nothing below it -- so the level never
passes the end of the array and the plan needs no end-of-plan case.

A rule's depth is therefore bounded by nothing but its length, and a rule with
ten thousand literals uses no more C stack than a rule with one.

### 2.1 State

The executor's state is exactly three things:

* **the binding frame**, one domain identifier per slot, at most 64 per D-7.
  A slot is written by the operation that binds it and read only by later
  ones, so a new match simply overwrites what the previous match left;
  nothing is saved and nothing is restored on backtracking, because nothing
  earlier in the plan ever reads a slot a later operation binds.
* **one state per operation**: the range of tuple identifiers its view
  covers, resolved for every operation when the plan starts, and the
  cursor, the half-open range a generator has still to visit. Tests keep
  no cursor.
* **the batch** of head tuples the `EMIT` has derived and not yet inserted,
  `specs/design-evaluation-cost.md` section 6.

None is allocated in the loop. The frame is a fixed array, each plan's
operation states are allocated with the plan, when the evaluation that runs
it begins, and the batch when the evaluation begins.

### 2.2 A generator never holds a tuple pointer

A `SCAN` reads a tuple through `relic_relation_tuple()`, binds from it, and
lets go. When it is resumed it fetches the next tuple by identifier. The
pointer is not kept across the operations below it, because one of them may be
an `EMIT` into the very relation being scanned, and an insertion can move the
storage the pointer points into.

An `EMIT` appends its head tuple to a batch, which is inserted when it fills
and when the plan ends, before the plan returns;
`specs/design-evaluation-cost.md` section 6 says why. So a batch can be
inserted while a scan of the head relation is still running.

The range a `SCAN` of the whole relation walks ends where the relation ended
when the plan started. Tuples the plan inserts lie beyond that end and are
not visited by any operation of the plan. They are not lost: they are
visited by the next plan to read the relation, in this pass or the next.
This keeps a plan's work bounded by the relations' sizes when it started,
makes what a plan reads independent of when its batches are inserted, and
is what a semi-naive delta range needs anyway.

A `LOOKUP` obeys the same rule. It walks the chain of tuples an index holds
for its key, holding only the next identifier on the chain, and stops at the
first one at or past that end; identifiers increase along a chain, so
nothing before that point is skipped. `specs/design-index.md` section 4
describes the walk.

Both are the `all` view of a relation. An operation of a semi-naive variant
over a member of its component reads through a narrower view whose end is
fixed when the round begins, section 4.1, and never reaches its own round's
insertions at all.

### 2.3 Comparisons

Per D-11 and architecture section 10.1:

* `=` and `!=` compare domain identifiers, which is exact because every value
  is interned: equal values have equal identifiers and nothing else does.
* `<`, `<=`, `>` and `>=` go through the value table. Integers compare
  numerically and symbols and strings bytewise. Values of different types are
  unordered, and an unordered comparison is false, so the binding is filtered
  out rather than reported as an error.

Verbosity level 2 reports the first cross-type ordering comparison per rule, so
that unintended mixing is discoverable. The library reports it only when the
context was created with `RELIC_F_NOTE_MIXED`, as a note located at the
comparison and naming the two types. A FILTER records where its comparison
was written for that purpose. Every compiled rule records its origin, the
program rule it was written as, which the rules a Magic rewrite generates
from it inherit and its semi-naive variants share, so each counts as the rule
it came from. The record that a rule has been reported lives on that program
rule for the life of the context, so the note is not repeated by the next
evaluation or query; a query's rules share one record, cleared when an
answer begins. Nothing is allocated for it. The check costs nothing unless
the switch is set and the comparison was unordered.

---

## 3. The naive fixed point

```text
for every component, in schedule order:
	if the component is recursive:
		repeat:
			for every rule of the component, in program order:
				run its plan
		until no insertion reported a new tuple
	else:
		for every rule of the component, in program order:
			run its plan
```

The schedule is the one [design-stratify.md](design-stratify.md) section 6
describes: strata in order and, within a stratum, strongly connected components
dependencies first. A component's rules read only its own predicates and those
of components already complete, so the fixed point of each component is taken
once, over relations that no later component changes. A component with no
recursion reads nothing it writes, and one pass over its rules is its fixed
point.

A fact is a rule with an empty body, so facts need no loading step of their
own: the first pass over their component inserts them. A fact is not planned.
Its head holds only constants, so the plan it would get is a single `EMIT` of
them; the pass inserts that tuple when the fact's turn comes, through the
insertion an `EMIT` makes, so a fact is ordered and counted exactly as that
plan would be. What it costs is its tuple, and nothing in the evaluation
arena, which holds the plans of rules with bodies only (section 9).

**It terminates** because every value an insertion can produce was interned
before evaluation began -- a rule's head holds constants and variables, and a
variable is bound only from a tuple -- so each relation can hold only finitely
many tuples, and an iteration that adds none is the last.

**It computes the least fixed point** because every tuple it inserts is
justified by tuples already present, and it leaves a component only when a full
pass over its rules adds nothing. Since no later component writes a predicate
an earlier one reads, every rule adds nothing once the last component is done,
which is the definition of a fixed point of the immediate-consequence operator.

Within an iteration a rule sees the insertions of the rules before it,
rather than only the state at the start of the iteration, and none of its
own: each of its operations reads the relation as it stood when the rule's
plan started, section 2.2. That changes how many iterations are needed and
not what they reach, and it is deterministic, because rules run in program
order and relations iterate in insertion order.

### 3.1 Why it stays

The naive evaluator is not the primary algorithm -- architecture section 26
forbids that -- but it stays in the library for the life of the project,
selected by `-X naive` and `RELIC_F_NAIVE`, per D-9. It is the differential
oracle: it shares the parser, the program model, the relation store and the
executor with everything else, so a disagreement between it and the semi-naive
evaluator isolates the difference between the two fixed-point loops rather than
implicating the whole pipeline. It is a dozen lines on top of the executor, and
it is covered by the same review and sanitizer requirements as the rest.

---

## 4. The semi-naive fixed point

Architecture sections 27 and 28 describe the algorithm: after the first round,
a recursive rule is evaluated only against the tuples its recursive literals
gained in the previous round.

```text
for every recursive component, in schedule order:
	delta(R) = empty at the first identifier, for every member R
	round 0: for every fact and every exit rule, in program order, run it
	while some member gained tuples in the last round:
		delta(R) = the tuples R gained in the last round
		for every delta variant, in planning order,
		    whose delta literal reads a member that gained
		    and whose every old view is nonempty:
			run the plan its delta's size selects
```

An **exit rule** is one with no positive literal over a member of its
component. Round 0 runs the facts and the exit rules only, so the first
delta of each member is the whole of it, and a recursive rule runs only as
its variants, from round 1 on. `specs/design-evaluation-cost.md` section 3.4
gives round 0, the round boundary and the invariant it keeps, and section 3.6
which variants a round runs.

**A round costs what changed.** A join over an empty operand is empty, so a
variant whose delta is empty, or one of whose `old` views is, derives
nothing wherever its plan places that literal. Skipping it changes nothing
but the cost, and running the rest in the order they were planned makes a
round do, operation for operation, what running every variant would. The
members that gained are recorded as plans insert into their heads -- every
insertion during a component's fixed point is into the head of the plan
making it, which is a member -- and the variants are grouped by the member
their delta literal reads when the component is planned, so a round touches
only those members and those variants. Without
it, a round costs the size of the component: a recursive cycle of *n*
predicates takes *n* rounds of one new tuple each, and running every variant
in every one of them is quadratic.

**A delta is a range of tuple identifiers**, per D-10. The store is
append-only, so the tuples a relation gained during one round are exactly
those with identifiers between its end before the round and its end after it.
Each member's delta is the previous delta's end to the relation's current end,
through `relic_relation_since()`; no delta is ever copied, and no delta has an
index of its own.

**The invariant this rests on**: within one fixed point, no relation is
compacted, has a tuple removed, or is renumbered. The relation store offers no
operation that could, so the invariant is a property of the interface
rather than a rule callers obey; the first feature that changes that --
incremental maintenance, or deletion -- must confront it before anything else,
because a range taken before such an operation silently names different tuples
afterwards.

### 4.1 Variants

For a rule with *k* positive body literals over members of its own strongly
connected component, *k* variants are planned. Every operation over a
relation reads through a **view**, `specs/design-evaluation-cost.md` section
3.1, and the views of variant *i* follow from body order alone: literal *i*
reads the **delta**, the tuples its member gained in the previous round; a
member literal written before it reads **old**, the member's tuples older
than its delta; a member literal written after it reads **current**,
everything the member held when the round began; and a literal over a
complete relation reads **all**. Section 3.2 of that document assigns them
and section 3.3 says what each operation does under each.

The views decide which tuples a literal can match, and the plan decides only
what each binding costs. The delta literal is therefore ordered like any
other literal and becomes a `SCAN`, a `LOOKUP` or a `MEMBER` of the delta by
its bound columns; a variant whose order puts another literal before the
delta keeps a second plan with the delta first, and each round runs the one
its delta's size selects. `specs/design-evaluation-cost.md` section 4 gives
the placement, the frontier a delta `LOOKUP` starts from, and the threshold
between the two plans.

A rule with no literal over its own component -- a fact, or an exit rule such
as `path(X, Y) :- edge(X, Y)` -- has no variant. Everything it reads was
complete before the component began, so round 0 derived everything it ever
will.

The evaluation keeps each member's delta and derives the ranges of its views
from it, and every operation's view is resolved when its plan starts. The plan
records only the view; which identifiers it denotes changes every round, and
the plan does not.

### 4.2 Why every derivation is made exactly once

Every way of satisfying a rule's body is derived in exactly one variant of
exactly one round: by the variant whose delta literal is the first member
literal, in body order, matched by a tuple of the latest round any of the
member tuples belongs to, in the round after that one.
`specs/design-evaluation-cost.md` section 3.5 gives the argument, both
completeness and uniqueness. It rests on two facts: the first delta of each
member is the whole of it, and no view of a member reaches past the end its
delta had when the round began, so nothing a round inserts is visible to any
variant of that round.

Evaluation proceeds stratum by stratum and, within a stratum, one strongly
connected component at a time in topological order. A component with no
recursion needs one pass. A component's plans and variants are built when the
component starts, never per iteration, so that the planner can measure the
relations of the components before it; `specs/design-planner.md` section 2
explains the choice.

### 4.3 Counters

The context counts the work evaluation does, cumulatively, for `relic_stats()`
and `-X stats`:

| Counter | Counts |
| --- | --- |
| iterations | rounds of a recursive component's fixed point, round 0 included |
| derivations | head tuples produced by an `EMIT`, facts included, new or not |
| tuples inserted | derivations that added a tuple |
| tuples scanned | tuples a `SCAN` read |
| tuples looked up | tuples a `LOOKUP` read from an index's chain; a frontier step, which positions a delta lookup and reads no tuple, is not one |

Iterations alone do not tell the two fixed points apart: on a chain, both take
as many rounds as the chain is long. What semi-naive evaluation saves is the
rederivation of what earlier rounds found, and that shows in the derivations
and the scans. The tests that guard against a regression to naive behaviour
therefore assert those. What an index saves shows in the tuples scanned and
looked up together: a join through an index reads the tuples that match, and
a join without one reads every tuple.

The counters are for understanding and comparing strategies. They depend on
the strategy and on the implementation, and are not part of any stable
interface.

---

## 5. Negation

A negated literal becomes `ANTI_MEMBER`: once its variables are bound, the tuple
it names is looked up, and the binding survives only if it is absent.
Stratification guarantees that the negated relation belongs to a lower stratum
and is complete by the time the test runs, so absence is meaningful. The
schedule evaluates every component after everything it depends on, negatively
as well as positively, so the guarantee holds with no check in the executor.

A negated literal never reads a member of its own component, because that
would be recursion through negation and the program would have been rejected.
It therefore never has a delta variant of its own, and in a delta variant of
another literal it tests the same complete relation every round.

An evaluator that cannot give a negated literal that guarantee must refuse to
plan one, naming the rule, rather than test absence against a relation that may
still grow.

---

## 6. Materialization and when it is current

**The relations of a context hold only what evaluation derived.** A fact is a
rule with an empty body, so even the facts are in a relation because the
pass over their component inserted them. The relations are therefore either
the fixed point of every rule the program has, or stale, and the context
records which by counting the
rules they are the fixed point of. Rules are only ever appended, because
checking is incremental and the syntax tree is append-only, so a count is
enough.

* `relic_materialize()` checks the program, and if rules have been added since
  the relations last reached a fixed point, **discards every tuple of every
  relation** and computes the fixed point again from nothing.
* A query run answers over everything loaded so far, so a caller that loads,
  queries, loads again and queries again gets answers over everything it
  loaded. It reads the fixed point when the relations hold it, and otherwise
  evaluates through the query's Magic rewrite, which discards the relations
  in the same way and leaves them holding part of a fixed point, counted as
  the fixed point of no rules.

Continuing from the tuples already held would be sound for a program without
negation, where a later fact can only add to what was derived, and unsound for
one with it, where a later fact can make a derived tuple false: `ready(build)`
stops holding once a later load gives `build` a dependency that is not done.
Discarding for every program, rather than only for one that negates, is what
keeps that question from being asked at all, and it is the rule architecture
section 54 already sets for `relic_fact_add()`: adding facts after evaluation
invalidates derived state. One rule covers both ways of changing the program.

The discard empties the relations in place, so a pointer to one stays valid,
and it happens before an evaluation begins, so no delta range can span it --
the obligation `specs/design-relations.md` section 4 places on any operation
that ends a span of the append-only store.

A failed evaluation leaves the count where the discard put it, so the relations
are stale and the next evaluation discards the part of a fixed point they hold
before starting again.

---

## 7. Answering a query

A query is a rule whose head is its output columns, per
`specs/design-plan.md` section 7, answered into the **answer relation** the
query object owns, per `specs/design-query.md`. Answering one is three steps:

1. **Evaluate what the query reads.** When the relations hold the program's
   fixed point, or under `RELIC_F_NO_MAGIC`, that is materializing the
   program as above, and the query rule is the one written. Otherwise the
   query is rewritten by Magic Sets, per `specs/design-magic.md`: the
   relations it reads completely are evaluated from the program's schedule,
   the rules the rewrite generates are evaluated from a schedule of their
   own, and the query rule is the rewritten one. A rewrite that outgrows its
   budget is abandoned, and the query answered from the materialized program
   instead.
2. **Run the query's plan once**, with the answer relation as its emit
   target. The answer is read by no rule, since it is no predicate's relation,
   so one pass computes it completely. Duplicate answers collapse on
   insertion, which is what gives a query set semantics.
3. **Stream the answer relation** to the caller's callback, one row per tuple,
   in insertion order.

```c
typedef int (*relic_row_cb)(void *, const relic_value_t *, size_t);

int relic_query_run(relic_t *, relic_query_t *, relic_row_cb, void *);
```

A row is the tuple's values converted to `relic_value_t`, borrowed for the
duration of the call. A callback that returns nonzero stops the stream, and the
run still reports success: the caller asked to stop, and nothing failed.

**No row is delivered until the answer is complete.** Every step that can fail
-- a limit, an allocation -- happens in steps 1 and 2, and step 3 cannot fail.
A query that exceeds a limit therefore produces no rows and an error, never a
truncated answer indistinguishable from a complete one, which is what
architecture section 63 requires.

The answer relation is emptied when the run ends, whether the rows were
delivered, the callback stopped them or the answer failed part way, so
outside a run it holds nothing and counts nothing against the limits. Running
the query again repeats the three steps and streams the same rows in the same
order.

---

## 8. Failure

Evaluation fails for two reasons, both reported where they happen:

* a configured limit refused an insertion, with the message of
  `specs/design-relations.md` section 9;
* an allocation failed.

Either stops the fixed point at once and returns -1. Planning a component can
fail for the same two reasons -- memory, or the memory limit reached while an
index is built over a complete relation or while the evaluation arena grows,
per `specs/design-planning-cost.md` section 7 -- and stops the evaluation at
that component, before it runs. The relation that refused the tuple is exactly as
it was, an index whose build failed was never published, every other relation
holds tuples that are all justified, and the evaluation arena is released.
Nothing needs unwinding, which is the payoff of the relation store's
refuse-before-modify ordering, and the next evaluation discards what the
failed one derived before starting again.

---

## 9. Memory

Plans and cursors live in the **evaluation arena**, which is reset when each
evaluation ends, successfully or not, and which is charged to the memory
limit while it lives, per D-18. A fact has no plan, per section 3, and a
pass finds its turn from the schedule, so a program of many facts costs the
arena nothing and the limit counts the facts once, as tuples. Tuples and
indexes live in relations, which are individually allocated and accounted
against the limits, per D-6.
Nothing is allocated per tuple and nothing is allocated inside the executor's
loop.

---

## 10. Determinism

Rules run in program order, relations iterate in insertion order, and plans
are a function of the source, so the tuples of every relation are inserted in
the same order on every run. Rows are streamed in insertion order. Two runs
over the same input therefore produce the same rows in the same order, which is
what the golden tests compare; `-s` exists for the caller who must not rely on
that.

---

## 11. What is deliberately absent

* **Recursion in the executor.** Section 2.
* **A separate fact-loading pass.** A fact is a rule with no body, inserted
  in its turn in the pass over its component.
* **Rows before completion.** Section 7.
* **Incremental maintenance.** A change to the program discards everything
  derived, per section 6, and architecture section 70 defers anything finer.
* **Parallel evaluation.** A context is single-threaded, per D-14.
