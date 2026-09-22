# relic plan design

**Status.** Design document for the plan representation: what a rule becomes
once the planner has decided how to execute it, what each operation means,
and how a query is turned into a rule so that it can be planned at all.
Forward-looking, like every document under `specs/`; `STATUS.md` records what
is implemented.

**Companions.** `specs/architecture.md` sections 11, 23, 25, 58 and 59;
`specs/design-program.md` for the compiled rule this consumes;
`specs/design-eval.md` for the executor that runs a plan;
`specs/design-relations.md` for the relations a plan refers to;
`relic-plan.md` decisions D-4, D-7, D-8, D-10 and D-12.

---

## 1. What this document settles

* what a plan is, and why it is data rather than code;
* the six operations, and what each one does to the binding frame;
* how a plan says, column by column, whether a value is matched or bound, so
  that the executor never asks;
* where a comparison or a negated literal goes when its variables are bound
  later than it is written;
* how a query becomes a rule over a synthetic predicate, and what that
  predicate is called;
* how long a plan lives and what it may hold on to.

It does not settle how the planner *chooses* among orderings and access paths.
That is a cost question, and it belongs to `specs/design-planner.md`. This
document fixes the shape every planner produces, so that choosing better is a
change in what is emitted and never in how it is run.

---

## 2. Why a plan exists at all

D-8 is the most important sequencing decision in the project: **the executor
is written once, against an ordered array of operations, and every later
optimization changes the array rather than the executor.** Secondary indexes,
body reordering, semi-naive delta variants and the Magic Sets rewrite are all
planner changes. None of them touches the loop that runs a plan.

That only works if the representation is rich enough to express all of them
and simple enough that the executor needs no knowledge of how it was chosen.
Both requirements point at the same thing: a plan is a flat, ordered list of
operations over relations, each of which either produces bindings, tests them,
or emits a tuple, and each of which states every decision in advance.

A plan is not a bytecode. There is no instruction pointer arithmetic, no
register allocation and no jump. The order of the array *is* the nesting of
the join, and architecture section 59 explains why nothing more is justified.

---

## 3. The operations

```c
enum relic_op_kind {
	RELIC_OP_SCAN,		/* every tuple of a relation, matching and binding */
	RELIC_OP_LOOKUP,	/* the tuples an index finds for a bound key */
	RELIC_OP_MEMBER,	/* is this fully bound tuple present? */
	RELIC_OP_ANTI_MEMBER,	/* is this fully bound tuple absent? */
	RELIC_OP_FILTER,	/* does a comparison of two bound values hold? */
	RELIC_OP_EMIT		/* insert the head tuple */
};
```

They fall into two families, and the distinction is what the executor's loop
is built on.

**Generators** can succeed more than once. `SCAN` walks a range of tuple
identifiers; `LOOKUP` walks the identifiers an index holds for a key. Each
success leaves new bindings in the frame, and asking again moves on to the next
candidate.

**Tests** succeed at most once. `MEMBER`, `ANTI_MEMBER` and `FILTER` read only
bound values, bind nothing, and either let the binding through or reject it.
`EMIT` is a test that always rejects: it inserts the head tuple and then
reports that there is nothing further down this path, which is what makes it
the last operation of every plan.

| Operation | Reads | Binds | Succeeds |
| --- | --- | --- | --- |
| `SCAN` | bound columns, constants | unbound columns | once per matching tuple |
| `LOOKUP` | the key columns, then as `SCAN` | unbound columns | once per matching tuple |
| `MEMBER` | every column | nothing | once or never |
| `ANTI_MEMBER` | every column | nothing | once or never |
| `FILTER` | both operands | nothing | once or never |
| `EMIT` | every head column | nothing | never |

`MEMBER` exists separately from `SCAN` because a literal whose every argument
is bound is a membership question, and the canonical full-tuple index of
`specs/design-relations.md` answers it in constant time. `ANTI_MEMBER` is the
same question with the answer inverted, which is the whole of negation once
stratification has guaranteed that the negated relation is complete; see
architecture section 31.

---

## 4. Terms: the binding decision is made once

Every column an operation touches is described by a **term**, and a term says
not only what the column is but what the executor must do with it:

```c
enum relic_term_action {
	RELIC_PLAN_CONSTANT,	/* must equal an interned constant */
	RELIC_PLAN_BOUND,	/* must equal a slot bound before this column */
	RELIC_PLAN_BIND,	/* binds its slot from the tuple */
	RELIC_PLAN_ANY		/* `_`: matches anything, binds nothing */
};
```

Whether a slot is bound at a given point in a plan is a static fact: it is
bound if an earlier operation, or an earlier column of the same operation,
binds it. The planner therefore knows, for every column of every operation,
which of the four cases applies, and writes that down. The executor carries no
bound-mask, never tests one, and has no "is this variable bound yet?" branch
in its inner loop.

The same variable twice in one atom, `edge(X, X)`, is two terms: the first
column binds `X` and the second requires the value it just bound. The executor
processes columns left to right, so that falls out without a case of its own.

`BIND` and `ANY` occur only in a generator. A test reads bound values and
constants and nothing else, and `EMIT` reads only what the rule's safety
guarantees is bound by the time it runs.

---

## 5. The binding analysis every planner performs

Whatever order a planner chooses, it walks it keeping one `uint64_t`: the set
of slots bound so far. Each literal is then turned into an operation by one
rule, applied at the point the literal is placed:

* a **positive literal** whose every argument is a constant or a bound slot
  becomes `MEMBER`;
* any other positive literal becomes a generator -- `SCAN`, or `LOOKUP` when
  some of its columns are bound and an index covers them -- and adds its
  variables to the bound set;
* a **comparison** becomes `FILTER`, and a **negated literal** becomes
  `ANTI_MEMBER`. Neither binds anything, so each must be placed where every
  slot it reads is already bound.

The rule's safety, as `specs/design-program.md` section 5 defines it, is
exactly what makes the last point always satisfiable: every slot a filter
reads is bound by some positive literal, so once every positive literal has
been placed, every filter can be. Safety was stated as a set computation
rather than as an ordering search precisely so that this placement needs no
search either.

### 5.1 The textual planner

The first planner is deliberately trivial. Its ordering remains available
afterwards behind `-X no-reorder`, and its access paths behind `-X no-index`,
as the references the optimized planner is compared against:

* positive literals are placed in the order they are written;
* each filter is placed at the **earliest** point at which every slot it reads
  is bound, and filters that become placeable at the same point keep their
  written order;
* there are no secondary indexes, so every generator is a `SCAN`.

Placing a filter early rather than at its written position is not reordering
in the sense of `-X no-reorder`. The written position of a comparison is not
necessarily one where its operands exist -- `N >= 100, size(X, N)` is a safe
body -- so *some* position other than the written one must be chosen for it,
and the earliest one is the only choice that is both always legal and never
worse. The positive literals, whose order is the one that governs the cost of a
join, are exactly as written.

### 5.2 Later planners

Each later stage replaces a piece of 5.1 and leaves the rest:

* **semi-naive variants** pin the delta literal first and scan only its range,
  per D-10, so that a delta never needs an index;
* **secondary indexes** turn a generator with bound columns into a `LOOKUP`;
* **greedy ordering** chooses which positive literal to place next by bound
  arguments, estimated cardinality and index availability, breaking ties by
  source position;
* **Magic Sets** rewrite the program before any of this runs, and the
  rewritten rules are planned by the same planner with no special case.

The filter placement of 5.1 and the operation choice of section 5 are common
to all of them. `specs/design-planner.md` specifies the cost model.

### 5.3 Delta variants

A semi-naive delta variant is the plan of a rule with one positive literal,
the **delta literal**, placed ahead of all the others. It is planned by the same
walk with that one change:

* the delta literal is placed first, before any other positive literal, and
  comparisons that its bindings make placeable follow it as usual;
* it always becomes a `SCAN`, marked as reading the delta, even when every
  argument is a constant. A `MEMBER` would consult the whole relation, and the
  point of the variant is to read only what the previous round added;
* every other literal is placed as it would be in the rule's ordinary plan,
  against the bindings the delta scan made.

```c
struct relic_op_atom {
	relic_relation_t	*relation;
	relic_plan_term_t	*terms;
	bool			 delta;		/* SCAN only */
};
```

The plan records *that* a scan reads the delta and not *which* tuples that is.
The range changes every round while the plan does not, so the evaluation holds
one range per predicate and a delta scan takes its range from there when it
starts; `specs/design-eval.md` section 4 describes how the ranges advance.

Pinning the delta literal first is what keeps a delta from ever needing an
index. It is scanned, never looked up, so the index machinery of a later
planner applies only to whole relations.

---

## 6. The plan

```c
struct relic_plan {
	relic_op_t	*ops;		/* ends with exactly one EMIT */
	size_t		 op_count;
};
```

An operation over a relation holds the relation itself rather than a
predicate identifier to resolve on every step. That is safe because a relation
never moves -- `specs/design-relations.md` section 2 -- so a plan may hold the
pointer for as long as the relation exists.

Three invariants hold for every plan, and the executor relies on them without
checking:

* **the last operation is `EMIT`, and it is the only one;**
* **every `BOUND` term reads a slot some earlier operation, or an earlier
  column of the same operation, binds;**
* **every test reads only constants and bound slots.**

A plan whose last operation is `EMIT` needs no end-of-plan case in the
executor: the deepest level always rejects, so the loop always backtracks.

### 6.1 Lifetime

A plan is built when a fixed point starts and discarded when it ends, in the
evaluation arena of `specs/design-memory.md`. It is never rebuilt per
iteration: replanning would chase cardinalities that change as the fixed point
proceeds, at a cost that is hard to bound, and would make `-e` output depend
on how many iterations ran.

A plan is not kept between evaluations either. It holds relation pointers and
is cheap to build, and a plan that outlived the evaluation it was built for
would have to be invalidated by whatever discards derived state. Building one
per evaluation makes that question disappear.

---

## 7. A query is a rule

Architecture section 11 turns every query into a rule over a synthetic
predicate whose arguments are the query's output columns:

```prolog
depends(X, Y), status(Y, todo)?
```

is compiled as

```prolog
$q0(X, Y) :- depends(X, Y), status(Y, todo).
```

and from then on it is planned, evaluated and stored like any other rule. The
answer to the query is the relation of `$q0/2`.

**The output columns are the named variables in order of first occurrence,
and `_` contributes none.** Variables are numbered into slots in exactly that
order already -- `specs/design-program.md` section 4 -- so the head is simply
every named slot in slot order. `_` in a positive literal is a wildcard with
no slot, and `_` anywhere else is unsafe and rejected, so nothing needs
skipping.

**A query is checked by the same safety computation as a rule.** Every head
variable must be bound by a positive literal, and the head is the list of
output columns, so "every output column is bound" needs no code of its own. A
variable that occurs only in a comparison or a negated literal is reported by
the ordinary message, worded for a query:

```text
<query>:1:1: error: variable X in the query is not bound by a positive body literal
```

The synthetic head counts as one occurrence of each output variable. That is
what it is, and it is why `path(a, X)?` produces no singleton warning: `X` is
mentioned once in the text but twice in the rule the text means.

**A query has at most 32 output columns.** A rule may hold 64 variables but a
predicate at most 32 arguments, and the synthetic head is a predicate. A query
with more named variables is rejected at the first one past the limit:

```text
<query>:1:40: error: the query has more than 32 output columns
```

### 7.1 Names

The synthetic predicate is named `$q` followed by a decimal serial number, per
D-12. `$` cannot begin or appear in an identifier the lexer accepts, so no user
predicate can collide with one, by construction rather than by a uniquifying
check. Arity is part of predicate identity, so the name alone would not need
to be unique; it is anyway, so that `-e` output can refer to a query by name.

The serial number counts the queries the context has compiled, in the order it
compiled them. Queries written in the program are compiled by `relic_check()`
in load order, so the first query in the first file is `$q0`. A query prepared
from text afterwards takes the next number. Identical sequences of operations
therefore produce identical names, which is what D-4 requires; a counter is
acceptable here where it is not for Magic predicates, because the query
serial is determined by the order of the caller's own operations rather than by
the internal order of a rewrite.

A rejected query takes no number.

### 7.2 Loaded and prepared queries

A query reaches the planner by one of two routes, and they meet at the
compiled rule:

* a query **written in the program** is compiled by `relic_check()` along with
  the rules around it, so an unsafe query makes the program invalid exactly as
  an unsafe rule does. Its compiled rule lives in the program arena.
* a query **prepared from text** is parsed and compiled when it is prepared,
  into an arena of its own that is released when the query is destroyed. The
  program must already have passed its check, so that the undefined-predicate
  sweep that follows speaks only about predicates the query introduced.

Either way the result is a compiled rule whose head is the answer predicate,
and nothing downstream can tell which route it took.

---

## 8. Determinism

The plan contributes three things to the reproducibility of a run:

* **the textual planner's order is a function of the source**, with ties
  between filters broken by written order;
* **query serial numbers follow the order of compilation**, which is load order
  and then the caller's order;
* **nothing in a plan is ordered by a pointer, a hash or a domain
  identifier.**

---

## 9. What is deliberately absent

* **A bytecode.** Architecture section 59 settles this: an array of operations
  is direct, debuggable and fast enough.
* **A runtime bound-mask.** Binding is static, so the planner decides it and the
  executor obeys.
* **Recursion in the plan.** A plan is flat; the nesting of the join is the
  order of the array, which is what lets the executor run it with a loop.
* **Plan caching across evaluations.** Section 6.1.
* **An operation for a comparison that binds.** Comparisons are filters, per
  architecture section 10: `X = 42` does not bind `X`. Arithmetic and
  assignment are post-v1.
