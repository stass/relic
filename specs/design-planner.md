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
`specs/design-evaluation-cost.md` sections 3 and 4 for the views a variant
reads and the placement of its delta literal;
`relic-plan.md` decisions D-4, D-8 and D-10.

---

## 1. What this document settles

* when a component's rules are planned, and why then;
* which relations the planner may measure and which it must guess about;
* the statistics a relation offers and how they stay correct;
* the estimate a literal is scored by, in integer arithmetic;
* the greedy ordering, its tie-breaks, and what it leaves alone;
* the two plans of a delta variant, and the threshold a round chooses
  between them by;
* the access path a positive literal gets;
* the `-X no-index` and `-X no-reorder` references;
* the static mode Magic Sets plans in;
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
counted against `max_memory`: it is at most a few bytes per tuple of one
relation the limit already admitted, briefly, so the limit bounds it through
the relation.

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

In a semi-naive delta variant the guess depends on the view the literal
reads, `specs/design-evaluation-cost.md` section 4.4. A member read through
`all` or `delta` estimates `RELIC_PLAN_UNKNOWN_TUPLES`; a member read
through `old` or `current`, beside its own delta, estimates
`RELIC_PLAN_UNKNOWN_TUPLES` times `RELIC_PLAN_DELTAS_HELD`, 10, before the
same division. That says only that a member is larger than its own delta,
which is true by construction, and it ranks the delta ahead of every other
member literal with the same bound columns.

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
left. `specs/design-planning-cost.md` computes the same order in one pass
per state of the bound set rather than one comparison per step, so that a
long body costs its length within a logarithm rather than its square.

Architecture section 23 lists index availability as the third preference.
Under section 5.2 it cannot tell two candidates apart: every literal with a
bound column gets an index, so among literals with the same number of bound
arguments either all have one or none does.

One thing is fixed before the greedy order starts, and it does not touch it:
**tests are placed at the earliest point every slot they read is bound**,
exactly as in the textual planner of `specs/design-plan.md` section 5.1,
after whichever positive literal makes them placeable.

**A delta literal takes part in the order** like any other positive literal,
scored by the estimate of its view (section 4), so a small complete relation
that binds the columns the delta would be looked up by goes before it, as it
would before any other literal. That is the shape a Magic rewrite produces,
a magic relation of a few bindings guarding a rule whose delta holds the
answers found so far for all of them.

**A variant may have two plans.** Whether the placed order is cheaper than
the delta first depends on the delta's size, which the planner cannot know
and which changes from round to round. So when the order places another
positive literal before the delta literal, the variant keeps two plans:

* the **placed plan**, the greedy order, whose **prefix estimate** is the
  product of the estimates the positive literals before the delta literal
  were placed by: the bindings that reach the delta literal in a round, each
  one lookup of the delta whatever it holds;
* the **delta-first plan**, the same walk with the delta literal placed
  before every other positive literal, whose **fan-out estimate** is the
  product of the estimates of the positive literals after it: the bindings
  each delta tuple leads to.

A round whose delta holds *D* tuples costs the placed plan about the prefix
and the delta-first plan about *D* times the fan-out, so the variant's
**threshold** is the prefix over the fan-out, in integer division, and each
round runs the placed plan when its delta holds more tuples than the
threshold and the delta-first plan otherwise. A prefix of zero, a relation
before the delta found empty, makes the threshold zero; otherwise a fan-out
of zero makes it one no delta exceeds. The products are 64-bit and saturate
rather than wrap, so the same program has the same threshold on every
platform. A plan that no delta would select is not built, and neither are
the indexes only it would read. When the order places the delta literal
first, the two plans coincide and there is one.
`specs/design-evaluation-cost.md` sections 4.3 and 4.4 give the reasoning
and the examples.

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
* `SCAN`, when none are.

A delta literal follows the same rule through the delta view: a `SCAN` of
the delta, a `LOOKUP` whose walk starts, through the key's frontier, at the
key's first tuple inside the delta, `specs/design-index.md` section 4.1, or a
`MEMBER` that holds only for a tuple of the delta. The delta is never
copied and has no index of its own; it reads the relation's.

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
| `-X no-index` | `RELIC_F_NO_INDEX` | no `LOOKUP`: a literal with bound columns is a `SCAN` of its view that matches them; a delta variant places its delta literal first and has that one plan, since a delta scanned after other literals would be scanned once per binding |
| `-X no-reorder` | `RELIC_F_NO_REORDER` | positive literals in written order, the delta literal in its written place, and one plan per variant |

Both leave everything else as it is: `MEMBER` still answers a fully bound
literal, because the canonical index is not a secondary one, and tests are
still placed as early as they can be. With both switches the planner is the
textual planner of `specs/design-plan.md` section 5.1, and a delta variant is
the textbook semi-naive scheme.

Each must give the default's answers, and `tests/equiv/strategies.sh` runs the
language corpus under each to show it.

---

## 7. The static mode

Magic Sets rewrite a query's program before most of it is evaluated, and
choose the order in which bindings flow through a rule body -- the sideways
information passing of architecture section 35 -- with the planner's
ordering, so that the rewrite has no private notion of body order. At rewrite
time the relations the query reads on demand are unknown; those it reads
completely have been evaluated, per `specs/design-magic.md` section 3.

That is the incomplete set of section 3 and nothing more: the rewrite plans
with every predicate read on demand marked incomplete, and the relations it
can measure are the complete ones. It asks for the order alone, starting from
the slots the head's bound columns bind, and the plan that evaluation later
emits is the same ordering followed by one operation per literal. There is
one ordering, used by the evaluator and by the rewrite, and the only
differences between the two uses are which relations are in the set and
which slots are bound at the start.

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

For the graph program of architecture section 72 and `-q 'path(a, X)?'`,
under `-a`, which answers the query from the whole fixed point:

```text
stratum 0, component 0: edge/2
    facts: 3
stratum 0, component 1 (recursive): path/2
    rule path(X, Y) :- edge(X, Y).
        scan edge(X, Y)
        emit path(X, Y)
    rule path(X, Z) :- edge(X, Y), path(Y, Z).
        delta path(Y, Z) when the delta holds more than 3 tuples:
            scan edge(X, Y)
            lookup delta path(Y, Z) key={0}
            emit path(X, Z)
        delta path(Y, Z) otherwise:
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
* Each other rule is shown as written. A rule of a component evaluated in one
  pass, an exit rule of a recursive component, and every rule under `-X naive`
  are each followed by its plan. A recursive rule under the semi-naive fixed
  point runs only as its variants and shows no plan of its own: each variant
  follows, headed by the literal that reads the delta.
* A variant with one plan is headed `delta` and its delta literal. A variant
  with two shows the placed plan, headed by the threshold its delta must
  exceed, `when the delta holds more than N tuples` (`tuple` when N is one),
  and then the delta-first plan, headed `otherwise`. Which plan a round chose
  is not printed, so that the lines are a function of the plans and not of how
  many rounds ran.
* A query is shown as the rule it compiles to, headed by its name,
  `$q` and a serial.
* A query answered through its Magic rewrite also shows the rewrite and the
  components of the rewritten rules, nested beneath it, in the layout
  `specs/design-magic.md` section 10 gives.

An operation is its kind followed by the literal it reads, written as it
would be in a program: `scan`, `lookup` with its key columns, `member`,
`anti-member`, `filter` with its comparison, and `emit`. An operation over a
member of the component follows its kind with the view it reads when that
view is `delta` or `old`: `scan delta`, `lookup delta`, `member delta`,
`scan old`, `lookup old`, `member old`. The `current` view, what a member
literal of a variant reads unless marked otherwise, and `all` are unmarked.
Variables are named as the rule names them, `_` is written as
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
* **Replanning during a fixed point.** Section 2. A variant's plans are built
  when its component is planned, with the indexes they read, and a round
  only chooses between them (section 5.1).
* **Multi-column statistics.** Architecture section 24 calls them unnecessary
  initially; the independence assumption is visible in section 4.
* **Estimates in `-e` output.** The plan says what was chosen; the estimates
  that chose it depend on constants that may change, and would make every
  golden plan move with them. A variant's threshold is the one figure
  derived from them that `-e` prints, because without it a reader could not
  tell which of the two plans a round runs.
* **Plans without evaluation.** A plan depends on the complete relations of
  the components before it, so it exists only once they are complete.
