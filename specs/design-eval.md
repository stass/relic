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

The executor's state is exactly two things:

* **the binding frame**, one domain identifier per slot, at most 64 per D-7.
  A slot is written by the operation that binds it and read only by later
  ones, so a new match simply overwrites what the previous match left;
  nothing is saved and nothing is restored on backtracking, because nothing
  earlier in the plan ever reads a slot a later operation binds.
* **one cursor per operation**, the half-open range of tuple identifiers a
  generator has still to visit. Tests keep no state.

Neither is allocated in the loop. The frame is a fixed array, and each plan's
cursors are allocated with the plan, when the evaluation that runs it begins.

### 2.2 A generator never holds a tuple pointer

A `SCAN` reads a tuple through `relic_relation_tuple()`, binds from it, and
lets go. When it is resumed it fetches the next tuple by identifier. The
pointer is not kept across the operations below it, because one of them may be
an `EMIT` into the very relation being scanned, and an insertion can move the
storage the pointer points into.

The range a `SCAN` walks is taken when it starts. Tuples inserted into the
relation while the scan runs lie beyond its end and are not visited by that
scan. They are not lost: they are visited by the next scan to start over the
relation, in this iteration or the next. This keeps a scan's work bounded by
the relation's size when it started, and it is what a semi-naive delta range
needs anyway.

A `LOOKUP` obeys the same rule. It walks the chain of tuples an index holds
for its key, holding only the next identifier on the chain, and stops at the
first one at or past the relation's end as it was when the lookup started;
identifiers increase along a chain, so nothing before that point is skipped.
`specs/design-index.md` section 4 describes the walk.

### 2.3 Comparisons

Per D-11 and architecture section 10.1:

* `=` and `!=` compare domain identifiers, which is exact because every value
  is interned: equal values have equal identifiers and nothing else does.
* `<`, `<=`, `>` and `>=` go through the value table. Integers compare
  numerically and symbols and strings bytewise. Values of different types are
  unordered, and an unordered comparison is false, so the binding is filtered
  out rather than reported as an error.

Verbosity level 2 reports the first cross-type ordering comparison per rule, so
that unintended mixing is discoverable.

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

A fact is a rule with an empty body, and its plan is a single `EMIT`, so facts
need no loading step of their own: the first pass over their component inserts
them.

**It terminates** because every value an insertion can produce was interned
before evaluation began -- a rule's head holds constants and variables, and a
variable is bound only from a tuple -- so each relation can hold only finitely
many tuples, and an iteration that adds none is the last.

**It computes the least fixed point** because every tuple it inserts is
justified by tuples already present, and it leaves a component only when a full
pass over its rules adds nothing. Since no later component writes a predicate
an earlier one reads, every rule adds nothing once the last component is done,
which is the definition of a fixed point of the immediate-consequence operator.

Within an iteration a rule sees the insertions of the rules before it, and of
its own earlier matches, rather than only the state at the start of the
iteration. That changes how many iterations are needed and not what they
reach, and it is deterministic, because rules run in program order and
relations iterate in insertion order.

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
	delta(R) = empty, for every member R
	round 0: for every rule, run its plan
	while some member gained tuples in the last round:
		delta(R) = the tuples R gained in the last round
		for every delta variant, in rule order then literal order:
			run it
```

**A delta is a range of tuple identifiers**, per D-10. The store is
append-only, so the tuples a relation gained during one round are exactly
those with identifiers between its end before the round and its end after it.
Each member's delta is the previous delta's end to the relation's current end,
through `relic_relation_since()`; no delta is ever copied.

**The invariant this rests on**: within one fixed point, no relation is
compacted, has a tuple removed, or is renumbered. The relation store offers no
operation that could, so today the invariant is a property of the interface
rather than a rule callers obey; the first feature that changes that --
incremental maintenance, or deletion -- must confront it before anything else,
because a range taken before such an operation silently names different tuples
afterwards.

### 4.1 Variants

For a rule with *k* positive body literals over predicates of its own strongly
connected component, *k* variants are planned; variant *i* reads literal *i*
from the delta and every other literal from the full relation. The delta
literal is placed first and is always a `SCAN` of its range, even when every
argument is a constant, so no index over a delta is ever needed. Some tuples
are derived more than once in a round; the destination relation discards the
duplicates. A refinement avoids them, reading the relations as they were before
the round for the literals ahead of the delta one; it saves work only for rules
with several recursive literals and is not obviously correct on first reading,
so the design is the simpler scheme until a measurement says otherwise.

A rule with no literal over its own component -- a fact, or a base case such
as `path(X, Y) :- edge(X, Y)` -- has no variant. Everything it reads was
complete before the component began, so round 0 derived everything it ever
will.

The delta ranges are held by the evaluation, one per predicate, and a delta
`SCAN` takes its range from there when it starts. The plan records only that
the scan reads the delta; which identifiers that is changes every round, and
the plan does not.

### 4.2 Why it is complete

Number the rounds from zero, and say a tuple of a member belongs to the round
that inserted it, or to round -1 if the relation held it before round 0.
Take any way of satisfying a rule's body, and let *m* be the latest round any
of its member tuples belongs to.

* If *m* is -1, round 0 ran every rule's full plan over relations that
  already held all of them.
* Otherwise round *m* inserted a tuple, so round *m* + 1 runs. In it, the
  variant whose delta literal is matched by a tuple of round *m* finds that
  tuple in its delta, and finds every other tuple in the full relations,
  whose scans start after round *m* ended.

Insertions within a round are visible to scans that start later in the same
round. That only adds derivations; the argument above does not depend on it.

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
| tuples looked up | tuples a `LOOKUP` read from an index's chain |

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
rule with an empty body, so even the facts are in a relation because a plan
inserted them. The relations are therefore either the fixed point of every
rule the program has, or stale, and the context records which by counting the
rules they are the fixed point of. Rules are only ever appended, because
checking is incremental and the syntax tree is append-only, so a count is
enough.

* `relic_materialize()` checks the program, and if rules have been added since
  the relations last reached a fixed point, **discards every tuple of every
  relation** and computes the fixed point again from nothing.
* A query run materializes first, so a caller that loads, queries, loads
  again and queries again gets answers over everything it loaded.

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

A query is a rule over its answer predicate, per `specs/design-plan.md`
section 7. Answering one is three steps:

1. **Materialize** the program, as above.
2. **Run the query's plan once.** The answer predicate never appears in a body,
   because its name contains `$` and nothing a program writes can, so one pass
   computes its relation completely. Duplicate answers collapse on insertion,
   which is what gives a query set semantics.
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

The answer relation outlives the run, like every relation, until a change to
the program discards it with the rest. Running the query again repeats the
three steps, finds nothing new to insert, and streams the same rows in the
same order.

---

## 8. Failure

Evaluation fails for two reasons, both reported where they happen:

* a configured limit refused an insertion, with the message of
  `specs/design-relations.md` section 9;
* an allocation failed.

Either stops the fixed point at once and returns -1. Planning a component can
fail for the same two reasons -- memory, or the memory limit reached while an
index is built over a complete relation -- and stops the evaluation at that
component, before it runs. The relation that refused the tuple is exactly as
it was, an index whose build failed was never published, every other relation
holds tuples that are all justified, and the evaluation arena is released.
Nothing needs unwinding, which is the payoff of the relation store's
refuse-before-modify ordering, and the next evaluation discards what the
failed one derived before starting again.

---

## 9. Memory

Plans and cursors live in the **evaluation arena**, which is reset when each
evaluation ends, successfully or not. Tuples and indexes live in relations,
which are individually allocated and accounted against the limits, per D-6.
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
* **A separate fact-loading pass.** A fact is a rule with no body.
* **Rows before completion.** Section 7.
* **Incremental maintenance.** A change to the program discards everything
  derived, per section 6, and architecture section 70 defers anything finer.
* **Parallel evaluation.** A context is single-threaded, per D-14.
