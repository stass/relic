# relic planner design

**Status.** Design document for the planner's choices: when a rule is
planned, what the planner knows about the relations it reads, how it orders a
body, how it chooses between a scan, a lookup and a membership test, and how
the plan it chose is shown to a person. Forward-looking, like every document
under `specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 23, 24, 35, 58, 60 and 61;
`specs/design-plan.md` for the shape every plan has, which this document
never changes; `specs/design-index.md` for the indexes a lookup reads;
`specs/design-eval.md` for the fixed points that run the plans;
`relic-plan.md` decisions D-4, D-8 and D-10.

---

## 1. What this document settles

* when a component's rules are planned, and why then;
* which relations the planner may measure and which it must guess about;
* the statistics a relation offers and how they stay correct;
* the estimate a literal is scored by, in integer arithmetic;
* the greedy ordering, its tie-breaks, and what it leaves alone;
* the access path a positive literal gets;
* the `-X no-index` and `-X no-reorder` references;
* the static mode Magic Sets will plan in;
* `relic_explain_set()` and the text of `-e`.

`specs/design-plan.md` fixes what a plan is. This document fixes which plan is
chosen. The executor is not mentioned again, because nothing here changes it.

---

## 2. When a rule is planned

**A component's rules are planned when the component starts**, after every
component before it in the schedule has reached its fixed point, and never
again during that evaluation. A query's rule is planned when the query runs,
after the program is materialized.

The reason is what the planner can know. Evaluation discards every relation
before it begins, and a fact is a rule, so before the first component runs
every relation is empty and no statistic says anything. When a component
starts, every relation it reads outside itself belongs to an earlier
component and is complete: its size and its distinct values are the ones it
will have while this component runs. Only the component's own members are
unknown, and they are empty.

Planning once per component, rather than once per round, is the rule of
`specs/design-plan.md` section 6.1: a plan chasing cardinalities that change
as the fixed point proceeds would cost an unbounded amount and would make
`-e` depend on how many rounds ran.

**A planning failure can follow work.** Planning a component can fail: memory
can run out, and building an index over a complete relation can reach the
memory limit. When it does, the components before it have already derived
their tuples. The evaluation fails as a whole all the same -- nothing is
answered, and the relations are stale, so the next evaluation discards them
and starts again -- so the difference from planning everything first is when
a failure is noticed, not what it leaves behind. A limit that fires while an
index is built fires at the same total a derivation would have reached a few
tuples later, and says so.

---

## 3. What the planner knows

A relation read by a rule is either **complete** or **incomplete** when the
rule is planned.

* A relation of an earlier component, and every relation a query reads, is
  complete. The planner measures it.
* A member of the component being planned is incomplete. It is empty, and
  its eventual size is what the fixed point is about to compute, so the
  planner uses documented constants for it.

The planner is told which relations are incomplete by a set of predicates,
and a relation outside the set is taken to be complete. A query passes the
empty set.

### 3.1 Statistics

A relation offers two statistics, as architecture section 24 asks:

* its **tuple count**, which it always holds;
* the **distinct values in each column**, computed on demand.

A distinct count is computed by walking the relation once with a scratch
identifier table of the values seen, and cached on the relation. It is
**valid for as long as the tuple count is unchanged**. The store is
append-only between discards, so a relation holding as many tuples as when a
count was taken holds exactly those tuples, and the count is still true; the
tuple count is the version, and no insertion has to remember to mark a
statistic stale. The one operation that can return a relation to an earlier
count with different tuples is the discard, and it clears the cache.

Counting on demand rather than maintaining counts on insertion is a matter of
frequency: planning happens once per component and insertion millions of
times.

The scratch table is released as soon as the count is known and is not
counted against `max_memory`, for the reason the evaluation arena that holds
plans is not: it is working state of one planning step, not storage that the
data keeps. It is at most a few bytes per tuple of one relation, briefly.

A relation's third statistic in architecture section 24, its available
indexes, needs no computing: section 5 builds the index a plan needs.

---

## 4. The estimate

Each positive literal is scored by an **estimate**: the number of tuples it
would produce for each binding reaching it. For a complete relation of *n*
tuples with bound columns *B*:

```text
estimate = n
for each column c in B, in column order:
	estimate = ceil(estimate / distinct(c))
```

which assumes that columns are independent -- architecture section 24 asks
for nothing finer. An empty relation estimates zero, which puts it first among
the literals binding as many arguments, and ends the join once it is reached.
A nonempty one never estimates below one, because the division rounds up.

An incomplete relation estimates `RELIC_PLAN_UNKNOWN_TUPLES`, 1000, divided
the same way by `RELIC_PLAN_UNKNOWN_DISTINCT`, 10, for each bound column. The
constants say that a relation being derived is likely larger than a small
table of facts and smaller than a large one, and that binding a column of it
narrows it by an order of magnitude. Neither is a measurement; both are
visible in the source and in this document, which is all D-4 asks of a
heuristic.

**The arithmetic is integer.** A floating-point estimate is deterministic on
most platforms and not on all -- x87 extended precision rounds differently --
and a plan that depended on it would make `-e` output differ between two
builds of the same source.

---

## 5. Choosing a plan

### 5.1 Order

Positive literals are placed one at a time. At each step every positive
literal not yet placed is scored, against the slots bound so far, by:

1. **bound arguments**, more first: the columns holding a constant or a
   variable an earlier operation bound;
2. **the estimate**, smaller first;
3. **source position**, earlier first.

The first literal by that order is placed, the bound set grows by what it
binds, and the step repeats. This is the greedy ordering of architecture
section 23 and is not a search: each step is a comparison of the literals
left, so a body of *k* positive literals costs *k*(*k* + 1)/2 scores.

Architecture section 23 lists index availability as the third preference.
Under section 5.2 it cannot tell two candidates apart: every literal with a
bound column gets an index, so among literals with the same number of bound
arguments either all have one or none does.

Two things are fixed before the greedy order starts, and it does not touch
them:

* **a delta literal is placed first**, per D-10, so that a delta is only ever
  scanned;
* **tests are placed at the earliest point every slot they read is bound**,
  exactly as in the textual planner of `specs/design-plan.md` section 5.1,
  after whichever positive literal makes them placeable.

For the body of architecture section 23, with `tiny/1` holding 10 tuples,
`medium/1` 1000 and `huge/2` 100 000:

```prolog
result(X) :- huge(X, Y), tiny(X), medium(Y).
```

nothing is bound at first and every literal has no bound argument, so the
estimates decide and `tiny(X)` is placed. `huge(X, Y)` then has one bound
argument and `medium(Y)` none, so `huge` follows, looked up on `X`, and
`medium(Y)` comes last, a membership test.

### 5.2 Access path

A positive literal becomes, at the point it is placed:

* `MEMBER`, when every argument is bound: the canonical index answers it;
* `LOOKUP`, when some but not all are, through **the index keyed on exactly
  its bound columns**, which the relation builds if it has none;
* `SCAN`, when none are;

and a delta literal is always a `SCAN` of the delta, as before.

The index is keyed on every bound column because that is the most selective
lookup there is, and because the alternative -- reusing an index over some of
the bound columns when one exists -- would make the plan of one rule depend on
which rules happened to be planned before it. The cost is memory: a relation
looked up under several binding patterns carries an index for each, as
`specs/design-index.md` section 8 counts.

---

## 6. The references

Two switches select the planner's own references, the other half of the
equivalences of architecture section 66.2:

| Switch | Flag | Effect |
| --- | --- | --- |
| `-X no-index` | `RELIC_F_NO_INDEX` | no `LOOKUP`: a literal with bound columns is a `SCAN` that matches them |
| `-X no-reorder` | `RELIC_F_NO_REORDER` | positive literals in written order after the delta literal |

Both leave everything else as it is: `MEMBER` still answers a fully bound
literal, because the canonical index is not a secondary one, and tests are
still placed as early as they can be. With both switches the planner is the
textual planner of `specs/design-plan.md` section 5.1.

Each must give the default's answers, and `tests/equiv/strategies.sh` runs the
language corpus under each to show it.

---

## 7. The static mode

Magic Sets rewrite a program before it is evaluated, and choose the order in
which bindings flow through a rule body -- the sideways information passing of
architecture section 35 -- with the planner's ordering, so that the rewrite has
no private notion of body order. At rewrite time the derived relations are
unknown.

That is the incomplete set of section 3 and nothing more: the rewrite plans
with every derived predicate marked incomplete, and the relations it can
measure are the complete ones. There is one ordering, used by the evaluator
and by the rewrite, and the only difference between the two uses is which
relations are in the set.

---

## 8. Explain

`-e` shows the plans evaluation runs, as architecture section 60 asks, so that
a person can see why a query is fast or slow.

### 8.1 Interface

```c
typedef void (*relic_explain_cb)(void *, const char *);

void relic_explain_set(relic_t *, relic_explain_cb, void *);
```

The callback receives one line at a time, without a newline, while
evaluation runs: a component's plans as soon as the component has been
planned and before it runs, and a query's plan before its rows. Without a
callback nothing is formatted.

The lines are what ran, not a description reconstructed afterwards. A query
run against a program already materialized prints only its own plan, because
nothing else was planned; `-a` followed by a query prints the program's plans
under `-a` and the query's under the query. Two runs over the same input print
the same lines.

The tool writes each line to standard error, as it does the `-X stats`
counters: both are about the run rather than its results, and standard output
stays the query's rows alone, parseable in any format. Under `-c` nothing is
evaluated and `-e` prints nothing, like `-F` and `-s`, which also have no
effect there.

### 8.2 Text

For the graph program of architecture section 72 and `-q 'path(a, X)?'`:

```text
stratum 0, component 0: edge/2
    facts: 3
stratum 0, component 1 (recursive): path/2
    rule path(X, Y) :- edge(X, Y).
        scan edge(X, Y)
        emit path(X, Y)
    rule path(X, Z) :- edge(X, Y), path(Y, Z).
        scan edge(X, Y)
        lookup path(Y, Z) key={0}
        emit path(X, Z)
        delta path(Y, Z):
            scan delta path(Y, Z)
            lookup edge(X, Y) key={1}
            emit path(X, Z)
query $q0(X) :- path(a, X).
    lookup path(a, X) key={0}
    emit $q0(X)
```

* A component is headed by its stratum, its position in the schedule,
  whether it is recursive, and its members.
* Its facts are counted rather than shown, because a component of a million
  facts would otherwise print a million plans of one `emit` each.
* Each other rule is shown as written, followed by its plan, and, when it is
  evaluated semi-naively, each delta variant headed by the literal that
  reads the delta.
* A query is shown as the rule it compiles to, over its answer predicate.

An operation is its kind followed by the literal it reads, written as it
would be in a program: `scan`, `lookup` with its key columns, `member`,
`anti-member`, `filter` with its comparison, and `emit`. A delta scan says
`scan delta`. Variables are named as the rule names them, `_` is written as
`_`, and constants as the text output format writes them.

### 8.3 Safe to print

Every line is printable ASCII. A string constant is written as a literal with
a backslash, a double quote and every byte outside printable ASCII escaped, as
`\\`, `\"` and `\xHH`, so a program cannot send a terminal escape sequence
through its plan, and a callback need not escape anything.

A line longer than 1023 bytes is cut and ends in `...`. A plan is formatted
into a fixed buffer rather than an allocated one, because explaining a plan
must not be able to fail the evaluation it describes; only a rule of hundreds
of literals, or a string constant of hundreds of bytes, reaches the limit.

---

## 9. Determinism

* Estimates are integers computed from exact counts and fixed constants.
* Ties are broken by source position.
* Indexes are keyed by mask and built in identifier order.
* Plans are built once per component, not per round.
* `-e` lines are a function of the plans.

So two runs over the same input produce the same plans, the same derivation
order and byte-identical `-e` output, which the golden tests compare.

---

## 10. What is deliberately absent

* **A join-order search.** Architecture section 23: greedy is enough, and
  explainable.
* **Replanning during a fixed point.** Section 2.
* **Multi-column statistics.** Architecture section 24 calls them unnecessary
  initially; the independence assumption is visible in section 4.
* **Estimates in `-e` output.** The plan says what was chosen; the estimates
  that chose it depend on constants that may change, and would make every
  golden plan move with them.
* **Plans without evaluation.** A plan depends on the complete relations of
  the components before it, so it exists only once they are complete.
