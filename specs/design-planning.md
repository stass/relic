# Planning

Planning is the pipeline stage between a checked program and its evaluation:
it turns each compiled rule into a plan, an ordered array of operations the
executor runs. This document covers the operations, the static binding
analysis that fixes every column's fate before anything runs, where tests,
assignments and aggregates go, the statistics and integer estimates behind the
greedy order, access paths and probe shapes, the structural alternatives of
semi-naive delta variants, queries as rules, the order the Magic Sets
rewrite borrows, what planning costs and charges, and the text of `-e`,
which only this document defines ([explain](#explain)).

Related: [architecture](architecture.md#planning),
[language](language.md#safety), [front end](design-frontend.md),
[storage](design-storage.md), [evaluation](design-eval.md),
[expressions](design-expressions.md), [aggregates](design-aggregates.md),
[Magic Sets](design-magic.md), [memory](design-memory.md),
[testing](design-testing.md), [recursive plan selection](design-recursive-planning.md).

## Model

### The plan is data, and the executor is written once

The executor runs an ordered array of operations and nothing else. Every
optimization -- secondary indexes, body reordering, existence checks,
semi-naive delta variants, the per-round choice between two plans, Magic
Sets -- changes what the planner emits, never how the executor runs it, so
each can arrive, and be switched off for comparison, without touching the
loop that runs a plan.

That requires a plan rich enough to express every optimization and simple
enough that the executor needs no knowledge of how it was chosen: a flat
list of operations over relations, each producing bindings, testing them
or deriving a tuple, and each stating every decision in advance. The order
of the array is the nesting of the join. A plan has no jumps, registers or
recursion; the executor runs it with an explicit-level loop
([evaluation](design-eval.md)).

### The operations

There are nine operations (see `lib/plan.h`). **Generators** can succeed
more than once, leaving new bindings each time. **Tests** read only
constants and bound slots and succeed at most once; two of them also bind a
slot. **`EMIT`** ends every plan.

| Operation | Role | What it does |
| --- | --- | --- |
| `SCAN` | generator | walks every tuple of its view, matching and binding columns |
| `LOOKUP` | generator | the same, over the tuples a secondary index holds under its bound key |
| `MEMBER` | test | is this fully bound tuple in the view? |
| `EXISTS` | test | does some tuple in the view agree on the bound columns? |
| `ANTI_MEMBER` | test | does no tuple agree on the named columns? (negation) |
| `FILTER` | test | does a comparison of bound values hold? |
| `ASSIGN` | test that binds | binds a slot to a value computed from bound ones |
| `AGGREGATE` | test that binds | folds a complete relation's rows for a bound key, then binds or compares the result |
| `EMIT` | end | derives the head tuple, then rejects |

A `LOOKUP` visits exactly the tuples a `SCAN` with the same terms would
match, in the same order, which is what lets `-X no-index` serve as a
reference. `EMIT` hands its tuple to the evaluation, which inserts in
batches, or to the query, which may stream it, and then rejects; since the
deepest level always rejects, the executor always backtracks from it and
needs no end-of-plan case.

`MEMBER`, `EXISTS` and `ANTI_MEMBER` ask one question -- does the relation
hold, in the view, a tuple agreeing with these terms on these columns --
with a full key, a partial key, and the answer inverted. Negation is
`ANTI_MEMBER` and nothing more, because stratification completes a negated
relation before any rule reading it runs
([language](language.md#negation)). `ASSIGN` rejects a binding when an
operand has the wrong type; an undefined result such as overflow fails the
evaluation instead ([language](language.md#evaluation-errors)).
`AGGREGATE` fails when its function has no value over the group or, when
its result slot was already bound, when the values differ; aggregate
meaning is in the [language](language.md#aggregates).

### Views

Every operation over a relation reads through a **view**, a range of tuple
identifiers: `all`, or, in a semi-naive delta variant, `delta`, `old` or
`current` of a member of the component. The planner writes into each
operation which view it reads, never which tuples: the ranges change every
round and the plan does not, so the evaluation resolves every view when a
plan starts. What each view holds, how a variant assigns them by body
order, and why that makes every derivation happen exactly once are
[evaluation](design-eval.md#the-semi-naive-fixed-point)'s.

### When planning happens

**A component's rules are planned when the component starts**, after every
earlier component has reached its fixed point, and never again during that
evaluation; a query's rule is planned when the query runs. Evaluation
discards derived relations before it begins, so at a component's start
every relation it reads outside itself is a base store or belongs to an
earlier component: complete, with its final size and distinct values. Only
the component's own members are unknown, and they are empty until seeded,
after planning.

A plan lives in the evaluation arena and is discarded when the evaluation
ends. An operation holds its relation directly rather than a predicate to
resolve on each step, which is safe because a relation never moves while
it exists ([storage](design-storage.md)), and names an index by its
position in the relation's list, which is fixed until the next discard,
rather than by its address. Facts and a rewrite's body-less seeds are
inserted without a plan; only rules with bodies are planned.

## Invariants

The executor relies on these without checking them:

* **The last operation is `EMIT`, and it is the only one.**
* **Every slot a bound term, a postfix program or a fold reads is bound by
  an earlier operation (a generator, an `ASSIGN` or an `AGGREGATE`) or by
  an earlier column of the same operation.**
* **Every test reads only constants and bound slots.**

The binding analysis adds three facts. A bind term occurs only in a `SCAN`
or `LOOKUP`. An any term occurs in those, in an `EXISTS` and in an
`ANTI_MEMBER`, for a column nothing reads, and neither is ever in a key
column. **Except under `-X no-exists`, every generator binds a slot
something reads**: a literal whose unbound variables are all dead is a
test.

The planner's bound set is a 64-bit mask over the rule's slots, and **it
only grows**: a positive literal, assignment or aggregate adds slots and
removes none, and other tests add nothing. A monotone set over 64 bits has
at most 65 states, which bounds the cost of ordering
([The ranking](#the-ranking)). The same width sets the 64-variable limit
([architecture](architecture.md#limits)): a fixed word keeps the frame a
plain array with no allocation in the join loop.

## Static binding analysis

### Terms

Every column an operation touches is a **term** saying what the executor
does with it (see `lib/plan.h`): a **constant** to match, a **bound** slot
to match, a slot to **bind** from the tuple, or **any** (`_` or a dead
variable: match anything, bind nothing). An `EMIT` has two more, **number**
and **text**, for computed values it must store ([Slot kinds](#slot-kinds)).

Whether a slot is bound at a point in a plan is static: it is, if an
earlier operation or an earlier column of the same operation binds it. So
the planner decides every column's action and writes it down, and **the
executor carries no bound mask and has no "is this variable bound yet?"
branch in its inner loop.** The same variable twice in an atom,
`edge(X, X)` with `X` unbound, is two terms: the first binds `X`, the
second is matched against it, and left-to-right processing finds loops
with no special case. The planner takes a literal's bound columns before
deciding its terms, so the second `X` does not become a key column.

### Classifying a positive literal

When a positive literal is placed, each column is, in order of precedence:

1. a **key column**: a constant, or a variable bound before the literal;
2. an **any column**: `_` or a dead variable;
3. a **binding column**: a live unbound variable, at its first column in
   the literal;
4. a **matched column**: a variable a binding column of this literal binds.

The precedence settles the two ambiguous cases. A variable bound before
the literal is a key column whatever its mention count, so `Y = 3, p(X, Y)`
keeps its equality; and a dead variable is never bound before its literal,
since nothing else holds it, so cases 1 and 2 never meet. The key is
exactly the columns bound before the literal. The literal becomes `MEMBER`
when every column is a key column; `EXISTS` when no column binds and some
column is an any column; and otherwise a generator, `LOOKUP` with a key
column and `SCAN` without.

### Slot kinds

A slot's kind is fixed by the operation that binds it:

* a **domain slot** holds a domain identifier; generators bind them, as do
  an `ASSIGN` of a constant or a domain slot and a `min` or `max` fold,
  whose result is a value the group holds;
* a **number slot** holds a computed integer beside the frame; an `ASSIGN`
  of arithmetic, of an integer function (`length`, `index`, `match`) or of
  another number slot binds one, as does a `count` or `sum` fold;
* a **text slot** holds a computed string, its bytes in the executor's text
  stack; an `ASSIGN` of a string function (`cat`, `substr`, `str`) or of
  another text slot binds one.

A call's result type comes from the table the parser checks arguments
against, so kinds are never asked at run time. An operation over a relation
reads a number or text slot through an ordinary bound term, its frame entry
being the value's identifier or zero when the context has assigned it none
([zero in the frame](design-expressions.md#slots-and-cells)); only `EMIT`
needs the value itself, through a number or text term.

**The kind is a fact about the plan, not the rule.** In
`p(X), Y = X + 1, q(Y)` the assignment is placed after `p`, so `Y` is a
number slot and `q(Y)` a `MEMBER` keyed on it; placing `q` first would make
`Y` a domain slot and the equality a filter. In `p(X), q(Y), X = Y` both
variables are bound by positive literals, yet once `p(X)` is placed only
`X` is bound, so the plan is `assign Y = X` and `member q(Y)`: one probe
per `X` instead of a scan of `q`. Hence an expression's runnable form is
built with the plan, not the rule.

### Postfix programs

The planner compiles a `FILTER`'s operands and an `ASSIGN`'s expression
into postfix programs as it emits the operation, because each slot read in
a program names its kind, and the kind is known only once the order is.
The program's shape, why it is flat, and the bound on its stack are in
[expressions](design-expressions.md#postfix-programs).

## Placing literals

The planner walks an order, keeping the bound set, and turns each literal
into an operation where it is placed: a positive literal as classified
above, adding its live variables to the bound set; an equality with a bare
unbound variable on one side and only bound slots on the other into an
`ASSIGN`, every other comparison into a `FILTER`; a negated literal into an
`ANTI_MEMBER` keyed on its columns that are not `_`
([Probe shapes](#probe-shapes)); and an aggregate into an `AGGREGATE` once
the slots it shares with the rule are bound, binding its result slot or
comparing against it if an earlier operation bound it.

### Tests go at the earliest legal point

Tests, assignments and aggregates go at the **earliest** point where every
slot they read is bound: before the first positive literal if ready
against the starting set, otherwise right after the positive literal that
made them ready. Placement runs in passes: each places, in written order,
what the bound set at the pass's start makes ready, and passes repeat while
an assignment or aggregate binds a new slot. So `D1 = D + 1, D2 = D1 * 2`
lands after whatever binds `D`, in that order whatever order it was
written in, and a literal an assignment makes ready waits for the next pass
rather than overtaking one written after it that was ready already. What
an equality binds is decided against the slots bound as it is placed, so of
two assignments of one variable ready at one point, the second becomes a
test of what the first bound.

```text
rule large(X) :- size(X, N), N > 100, N <= 200, 1 < 2.
    filter 1 < 2
    scan size(X, N)
    filter N > 100
    filter N <= 200
    emit large(X)
```

This is not reordering in the sense of `-X no-reorder`. A comparison's
written position need not have its operands -- `N >= 100, size(X, N)` and
`not done(X), todo(X)` are safe bodies -- so some position must be chosen,
and the earliest is the only one always legal and never worse for a
filter, which rejects bindings before they fan out.

**Safety makes this search-free.** Every slot a test reads is bound by a
positive literal or by an assignment the safety closure reaches, so once
every positive literal is placed every assignment can be, and then every
test; placement never gets stuck or backtracks. Why safety needs no
ordering search is the [language](language.md#safety)'s.

### Aggregates: earliest is not always cheapest

An aggregate goes with the tests, at the earliest point its shared slots
are bound: `N = count { ... }, N > 3` places the fold as soon as its key is
bound and the filter right after. Its result joins the bound set, so a
positive literal keyed on it has a bound argument and is scored and
accessed accordingly. For a filter earliest is never worse; for a fold it
can be, since a fold whose result only the head reads is done before later
literals reject bindings that never reach the head. Placing it later would
need group-size estimates the planner does not have. Placement changes the
work, never the answer.

A fold reaches its rows as a positive literal with the same key columns
would: the whole relation when there is no key, the index on exactly the
key columns when there are some, one probe of the canonical index when
every column is a key column. The key columns are a group relation's
leading columns, or, for an atom folded in place, whichever columns hold
the shared variables -- a set, not a prefix, as indexes are keyed on sets.
The case is written into the operation. Under `-X no-index` the fold walks
the whole relation matching key columns, so the reference has no index the
default lacks.

## Dead variables and existence checks

### A binding nobody reads multiplies the join

A generator succeeds once per tuple it matches. If nothing later reads the
columns it binds, every match continues the join with the same frame, and
everything after runs once per match for the same result:

```prolog
p(a, x).
p(b, x).
s(X) :- p(_, X), p(_, X), p(_, X).
```

As generators, the second and third literals each match both tuples, so
`EMIT` runs 2^3 times to derive `s(x)` once, and 2^k times with k such
literals. No limit catches it, since limits bound what is held, not what is
read. Each such literal's answer is a yes or a no, which `EXISTS` asks once:

```text
rule s(X) :- p(_, X), p(_, X), p(_, X).
    scan p(_, X)
    exists p(_, X) key={1}
    exists p(_, X) key={1}
    emit s(X)
```

### A dead variable is one the rule mentions once

**A variable is dead when the rule mentions it exactly once, head
included, and that mention is in a positive literal.** `_` is the nameless
dead variable, so `p(X) :- e(X, _Y)` and `p(X) :- e(X, _)` are one rule to
the plan. The singleton warning reports a dead variable not spelled with a
leading `_`; a leading underscore says the column is ignored, and the plan
takes it at its word, warned or not.

**Why one mention is the whole condition.** Take a variable held by a
positive literal L and unbound when L is placed in some order. No literal
before L holds it: a positive literal would have bound it; a comparison,
negated literal or aggregate key waits until its slots are bound; and an
assignment or aggregate with it as result binds it. So "unbound at L and
read by nothing after L" means "held by L and nothing else", and by L once,
since a second column of L would read what the first bound: one mention.
Conversely a variable mentioned once, in a positive literal, is bound by
nothing else and read by nothing. The two conditions are the same set, and
the second depends only on the rule, so the planner computes it once, as a
64-bit **dead mask**, before ordering.

Mentions are counted by one walk over the rule's literals: a positive
literal mentions the variable in each column; a negated literal each named
argument; a comparison every slot of both sides; an aggregate its result
and shared slots (its body's own variables belong to its group rule); the
head each column. The walk reads the literals, which are what the plan
reads, not the occurrence record the singleton warning uses, because a rule
the Magic rewrite generates carries a synthetic count there. A query's
synthetic head holds every named variable, and a group rule's head every
variable of its body, so neither has dead variables other than `_`.

### Existence checks never change the order

The bound set grows, in the order and in the plan alike, only by live
slots. Three facts make the order the same as if dead variables were bound:

* a literal's score reads the bound set only through its own columns, and a
  dead variable is in no other literal;
* had dead slots been bound, a placement that grew the set by dead slots
  alone would have ended a ranking pass; recomputing the ranking then would
  sort the same scores by the same key, which ends in body position, into
  the same order the unbroken pass continues with;
* tests are offered places when the set grows, but no test reads a dead
  slot.

So the order is the same with or without existence checks. A delta variant
may hold a literal as a check in one of its plans and a generator in the
other, since different literals precede it. A check succeeds exactly when
the generator it replaces would succeed at least once, and that generator's
bindings are read by nothing, so the same tuples are derived, each fewer
times; only the `derivations` counter differs.

## Statistics and estimates

A relation a rule reads is **complete** when the rule is planned -- a base
store, a relation of an earlier component, or anything a query reads -- and
is measured; or **incomplete** -- a member of the component being planned,
empty when planned, its size what the fixed point is about to compute --
and is guessed. The planner is told which predicates are incomplete; a query
passes the empty set.

A relation offers its **tuple count** and the **distinct values of each
column**, counted on demand by one walk with a scratch identifier table and
cached. **The tuple count is the cache's version**: the store is
append-only between discards, so a relation with as many tuples as when a
count was taken holds exactly those tuples. No insertion marks a statistic
stale; clearing a relation, the only way back to an earlier count with
different tuples, clears the cache. Counting on demand suits the
frequencies: planning happens once per component, insertion millions of
times. The scratch table is freed at once and not charged to the memory
limit, being at most a slot per tuple of a relation the limit admitted.

A positive literal is scored by an **estimate** of the tuples it produces
per binding reaching it. For a complete relation of n tuples with bound
columns B:

```text
estimate = n
for each column c in B, in column order:
    estimate = ceil(estimate / distinct(c))
```

This assumes independent columns, which is good enough for ordering. An
empty relation estimates zero, so it goes first among literals binding as
many arguments and ends the join when reached; a nonempty one never
estimates below one. An incomplete relation is guessed at a fixed tuple
count, narrowed by a fixed factor per bound column: bigger than a small
table of facts, smaller than a large one. A non-delta member read through
`old` or `current` receives the fixed multiplier named in `lib/plan.h`.
These guesses rank the remaining literals during ordinary greedy walks.
They are not measurements.

The distinguished delta is placed structurally when its candidate plans
are constructed, so its guessed size cannot remove an alternative. The
round's choice uses actual view sizes and existing index key counts, as
specified in [recursive plan selection](design-recursive-planning.md).
That selector never requests fresh distinct-column scans.

**Estimates are integers.** Floating point is deterministic on most
platforms but not all -- x87 extended precision rounds differently -- and
plans depending on it would make `-e` and derivation order differ between
builds of one source. Products of estimates are 64-bit and saturate rather
than wrap.

## Ordering

### The greedy order

Positive literals are placed one at a time. Each step scores the unplaced
positive literals against the current bound set and places the least by:
**bound arguments**, more first; then **the estimate**, smaller first;
then **body position**, earlier first. The bound set grows by the
literal's live variables, ready tests are placed, and the step repeats --
a comparison per step, not a search, so cheap, deterministic and
explainable. With `tiny/1` holding 10 tuples, `medium/1` 1000 and
`huge/2` 100 000:

```text
rule result(X) :- huge(X, Y), tiny(X), medium(Y).
    scan tiny(X)
    lookup huge(X, Y) key={0}
    member medium(Y)
    emit result(X)
```

Index availability cannot serve as a further preference: every literal with
a bound column gets its own index, so among literals with as many bound
arguments either all have one or none does.

### The ranking

Rescanning the body at each step costs N scores for each of N steps; a
rule of thousands of literals reading one recursive predicate would spend
seconds planning before deriving anything. Everything a step asks depends
only on the bound set and on inputs fixed while a plan is ordered: a score
on the literal, the bound set and statistics nothing changes during
ordering (nothing inserts, and a relation looked up for statistics is
created empty if absent, which changes no score); a test's readiness on
whether its slots are in the bound set. So between placements that leave
the bound set unchanged, no score changes and no test becomes ready.

The order is therefore computed in **passes**, one per state of the bound
set. A **ranking** is the unplaced positive literals sorted by the key
against one bound set. From the caller's starting set -- empty for a
rule's plan, the head's bound columns when the Magic rewrite asks:

1. Place the tests ready against the starting set. In a delta-first order,
   place the delta literal and then the tests it made ready; otherwise the
   delta literal is ranked like any positive literal.
2. If no positive literal is left, stop. Otherwise score the unplaced ones
   and sort them into the ranking.
3. Place the first of the ranking and drop it.
4. If the bound set is unchanged and the ranking not empty, go to 3.
5. Otherwise place the tests the growth made ready, in written order, and
   go to 2.

Under `-X no-reorder` step 2 sorts by body position and scores nothing.

**Why this is the greedy order.** The greedy order places the least
unplaced literal against the current set, which is the first of a ranking
computed against it. Placing it without changing the set leaves every
other key as it was, so the least of the rest is the next of the ranking;
when the set changes, the ranking is recomputed, which is the step-by-step
comparison starting over. The key ends in body position, distinct per
literal, so the sort has exactly one result and the two procedures cannot
diverge on ties. A score that fails for lack of memory fails the same
literal at the same point in both, because a pass scores in body order.

**What it costs.** A pass scores at most N literals, sorts them in
N log N comparisons and scans N tests. Every pass after the first follows a
growth of the bound set, so there are at most 65, and one order costs at
most 65 x N log N rather than N squared. A short body that binds at every
placement still takes a pass per literal, within a logarithm of a rescan;
the bound matters for long bodies, whose set must stop growing long before
they end. The ranking is scratch, freed when the plan is built.

A plan of N literals holds N + 1 operations, so **planning time is bounded
by the size of the plans produced**, within a logarithmic factor. A delta
variant decides at most two orders and builds at least one plan, so the
bound holds within a factor of two. What remains multiplies plans, and
every copy is output the evaluation uses: a variant per body literal
reading a member of the component, a copy of a rule per Magic adornment.
Bounding planning time thus reduces to bounding the memory plans occupy,
which the memory limit does ([What planning
charges](#what-planning-charges)).

## Access paths

A candidate records an index requirement by relation and key mask.
Preparing an executable plan builds or reuses that index and binds its
position, so an executable plan never names a missing index. Ordinary and
delta-first plans are prepared at component start; an alternative's extra
indexes are acquired at its first selection, before the round executes
([recursive plan selection](design-recursive-planning.md#preparing-selected-plans)).
The index is keyed on every bound column because that is the
most selective lookup there is, and because reusing an index on a subset
of the bound columns would make one rule's plan depend on which rules were
planned before it. The price is an index per binding pattern, each charged
to the memory limit. A literal keyed on a slot an assignment or aggregate
bound, of any kind, has a bound argument and looks up through that slot's
frame entry; with `n` no larger than `edge`:

```text
rule succ_edge(X, Z) :- n(X), Y = X + 1, edge(Y, Z).
    scan n(X)
    assign Y = X + 1
    lookup edge(Y, Z) key={0}
    emit succ_edge(X, Z)
```

### Probe shapes

`EXISTS` and `ANTI_MEMBER` are **probes**: whether the relation holds,
inside the view, a tuple agreeing with the key, the answer kept or
inverted. An `EXISTS` is keyed on its bound columns, an `ANTI_MEMBER` on
its named columns (those that are not `_`, all bound where it is placed);
every other column is an any term. The key's shape decides how the
question is asked:

| Key | Shape | How it is answered |
| --- | --- | --- |
| every column | full | the canonical index finds the tuple; it holds when that tuple lies in the view |
| some columns | partial | the index on exactly those columns: the key's first tuple from the view's start exists and lies before the view's end |
| no column | empty | a scan of the view for the first tuple the terms match; with every term `_`, whether the view is not empty |

An `EXISTS` is never full, since a fully bound positive literal is planned
as `MEMBER`; an `ANTI_MEMBER` takes all three shapes. The empty shape's
scan is also how a partial key is asked under `-X no-index`, below.

**The partial probe is right because of chain order.** An index chains
each key's tuples in increasing identifier order, so the first chain tuple
from the view's start is the lowest there, and some tuple of the key lies
in the view exactly when that one is below the view's end. The probe
starts where a `LOOKUP` through the same view starts: at the chain's head
for `all`, `old` and `current`, and for `delta` at the key's first tuple at
or past the delta's start, found through the key's frontier, a position
that only moves forward because deltas only advance
([storage](design-storage.md)). So a check of a delta asks its index what a
lookup of the delta asks, and an `ANTI_MEMBER` is the `all` case. One
operation per probe kind, its key saying which shape, keeps the executor's
dispatch and explain's text parallel to `MEMBER`, `LOOKUP` and `SCAN`.

A negated literal's `_` is a column the probe does not read and takes no
slot, since the compiler treats a negated literal's arguments as **probed**
positions ([front end](design-frontend.md#variables-and-slots)). So
`not order(_, C, _)` behaves exactly like negating a helper relation
projected onto its named columns ([language](language.md#negation)). The
random suite checks that identity by rendering such literals through
helper rules and comparing answers ([testing](design-testing.md)).

**Without indexes.** Under `-X no-index` a partial probe cannot become a
different operation, its answer being a yes or a no, so it is planned with
an empty key and asked by the empty shape's scan, whose terms still match
the named columns. A test has no cursor to resume from, so the scan
charges every tuple it reads to the executor's **quantum**, the work it
does between looks at the interrupt flag, and finishes before the executor
may pause: a call overruns its quantum by at most one such scan
([interruption](design-eval.md#interruption)). It does no
more work than the generator it replaces, which read at least as far to
find the same first match. Probe counters depend only on the bindings met,
so they are identical at every thread count.

## Delta variants

A rule of a recursive component evaluated semi-naively runs as **delta
variants**, one per body literal reading a member of the component. The
distinguished literal reads its member's `delta`; other member literals
read `old` or `current` according to their original body positions. Plan
order cannot change that assignment.

Each variant has a counted family of at most two plans, built once when
its component starts:

* **delta-first**, with the distinguished literal pinned before the other
  positives, followed by ordinary greedy ordering;
* **input-first**, with another positive preferred over the delta when
  their bound-column ranks tie, followed by cardinality and source-position
  ranking among the other literals.

Identical complete orders collapse before emission. Otherwise both plans
are retained, with access requirements and work recipes; an estimated cost
does not prune a candidate, and retaining one does not build its extra
indexes. A fully bound delta can still be the first literal in both walks
because bound-column rank takes precedence.

Before any item of a round executes, the calling thread estimates each
retained plan's complete work from its operation views and direct index
key counts, including acquisition work and provisional counts for missing
indexes. It selects the lower estimate, preferring delta-first on a tie.
All choices precede preparation of the selected plans' access paths.
Estimation is a pure function of const inputs, uses integer arithmetic,
allocates nothing and reads no tuples. Reference modes retain one plan:
textual order under `-X no-reorder`, including its combination with `-X no-index`, and
delta-first under `-X no-index` alone.

The algorithm, statistics ownership, cost equations and correctness
argument are defined in [recursive plan selection](design-recursive-planning.md).
The [explain contract](#explain) below defines how the family is printed.

## Queries as rules

A query is compiled into a rule over a synthetic head of its output
columns, then planned and evaluated like any rule:
`depends(X, Y), status(Y, todo)?` becomes
`$q0(X, Y) :- depends(X, Y), status(Y, todo).` The head names no
predicate: the rule is answered into a relation the query object owns
([evaluation](design-eval.md)).

* **Output columns** are the named variables in order of first occurrence;
  `_` adds none. Slots are numbered in the same order, so the head is
  every named slot in slot order.
* **The safety check is the rule check**, so "every output column is
  bound" needs no code of its own; an unbound variable gets the ordinary
  message worded for a query, `variable X in the query is not bound by a
  positive body literal or an assignment`. The synthetic head counts as one
  occurrence of each output variable, which is why `path(a, X)?` draws no
  singleton warning.
* **At most 32 output columns**, because the head is a relation's tuple; a
  query with more named variables is rejected at the first past the limit.
* **Names.** A query is `$q` and a decimal serial, which never collides with a
  name a program writes ([naming](architecture.md#naming)). The name is
  formatted when printed and is neither a symbol nor a predicate. The serial
  counts compiled queries in order: the check compiles the program's queries
  in load order, so the first in the first file is `$q0`, and a query prepared
  from text later takes the next number; a rejected query takes none. A
  counter is acceptable here, unlike for Magic predicates, because it follows
  the caller's own sequence of calls.
* **Two routes meet at the compiled rule.** A query in the program is
  compiled with the rules around it, so an unsafe one makes the program
  invalid; a query prepared from text is compiled when prepared, after the
  program has passed its check, into its own arena. Nothing downstream can
  tell them apart.

A query reads only complete relations, so it is planned with the empty
incomplete set when it runs. Its `EMIT` may deliver rows as they are
derived, shown as `stream`, under conditions in
[evaluation](design-eval.md). Streaming relies on every test passing a
binding at most once, which `EXISTS` does, so `node(X), edge(_, X)?`,
whose last literal is a check, streams.

## Static planning for Magic Sets

The Magic Sets rewrite chooses how bindings flow through a body (sideways
information passing) by asking the planner for an **order alone**, so it
keeps no notion of body order of its own. The order is always decided
before any operation is built: a first walk places literals and records
what each binds, a second replays it into operations. The first walk builds
no index and emits nothing, which is what the rewrite asks for.

At rewrite time the relations the query reads completely have been
evaluated and can be measured; those read on demand cannot. That is the
incomplete set and nothing more: the rewrite marks every on-demand
predicate incomplete and starts from the slots the head's bound columns
bind. There is one ordering for evaluation and rewrite alike; they differ
only in which relations are incomplete and what is bound at the start.

Dead variables do not disturb the rewrite: a call can hold one, but none is
bound at any call, since nothing binds it before the literal that holds it,
which is the call. So each call's bound columns, its adornment and the
variables live after it are unaffected, and the rewritten rules are planned
by the same walk, with existence checks where their bodies have dead
variables and no case of their own.

## References

Three switches select the planner's references, each held to the default's
answers by the equivalence suite ([testing](design-testing.md)):

| Switch | Effect |
| --- | --- |
| `-X no-index` | no `LOOKUP`: a literal with bound columns is a `SCAN` of its view matching them; a partial probe scans; a variant puts its delta first and has one plan |
| `-X no-reorder` | positive literals in written order, the delta in its written place, one plan per variant |
| `-X no-exists` | no `EXISTS`: the dead mask is empty, so a literal binding nothing the rule reads is a generator and a named dead variable is bound |

Together, `-X no-index` and `-X no-reorder` are the textual planner.

## Explain

`-e` shows the plans evaluation runs, so a person can see why a query is
fast or slow. This section is the single definition of its text; feature
documents show examples and point here.

### Delivery

A library caller installs a callback with `relic_explain_set()`; it gets
one line at a time, without a newline, while evaluation runs: each
component's plans once planned and before they run, and a query's rewrite
and plan before its rows. With no callback nothing is formatted. Like a
diagnostic callback, it may read through the context and any other call it
makes is refused ([library interface](design-api.md)).

**The lines describe the plans evaluation builds.** A query against a
program already materialized prints only its own plan, because nothing
else was planned. Which of a variant's two plans a round chose is not
printed, so the lines depend on the plans and not on how many rounds ran.
The command line writes them to standard error, like `-X stats`, since both describe
the run rather than its results; standard output stays the rows alone.
Under `-c` nothing is evaluated and `-e` prints nothing. The text is for
people and may change between releases; within a build it is a
deterministic function of the input.

### Layout

For `edge(a, b). edge(b, c). edge(c, d).` with
`path(X, Y) :- edge(X, Y).` and `path(X, Z) :- edge(X, Y), path(Y, Z).`,
`relic -a -e -q 'path(a, X)?'` prints:

```text
stratum 0, component 0 (recursive): path/2
    rule path(X, Y) :- edge(X, Y).
        scan edge(X, Y)
        emit path(X, Y)
    rule path(X, Z) :- edge(X, Y), path(Y, Z).
        delta path(Y, Z):
            choose lowest estimated work; ties: delta-first
            plan delta-first:
                scan delta path(Y, Z)
                lookup edge(X, Y) key={1}
                emit path(X, Z)
            plan input-first:
                scan edge(X, Y)
                lookup delta path(Y, Z) key={0}
                emit path(X, Z)
query $q0(X) :- path(a, X).
    lookup path(a, X) key={0}
    stream $q0(X)
```

Each nesting level is four spaces.

* **A component** is headed `stratum S, component C: ` and its members as
  `name/arity`, with `(recursive)` after the position when it is.
  Components keep their schedule positions, so one not evaluated leaves a
  gap. A predicate of facts alone is in no component and is not shown.
* **`facts: N`** follows when N is not zero: the facts in the members' base
  stores plus the component's body-less rules, such as a rewrite's seeds.
  A million facts would otherwise print a million lines.
* **A rule** is `rule` and the rule in the language's syntax, printed from
  the compiled rule under its author's names. A rule run in one pass, an
  exit rule of a recursive component and every rule under `-X naive` are
  followed by their plan. A recursive rule under semi-naive evaluation runs
  only as its variants and has no plan of its own.
* **A variant** is headed `delta`, its delta literal and `:`. With one
  plan, its operations follow at one greater indentation. With two, the
  next indentation contains the line
  `choose lowest estimated work; ties: delta-first`, followed by
  `plan delta-first:` and its operations one level deeper, then
  `plan input-first:` and its operations at that same deeper level.
  These names describe structural ordering policies. The estimated work
  uses [the recursive selector's equations](design-recursive-planning.md#estimating-work).
  Runtime costs and selected-plan traces are not printed: a family has
  the same text regardless of the number of rounds that execute it.
* **A query** is `query` and its compiled rule, the head named `$qN`. Its
  own aggregate group rules appear under that heading with their plans,
  before the query's plan; a program's group rules appear under their own
  components.
* **A Magic rewrite** is headed `magic rewrite of` and the query's rule,
  then `adorn p/2 bf` lines (`, subsumed by p/2 ff` for a subsumed one),
  `seed` lines, an optional `read completely:` line, the rewritten
  program's components one level deeper, and the rewritten query. An
  abandoned rewrite prints `abandoned magic rewrite of` and the query's
  rule, then the whole program's components. [Magic Sets](design-magic.md)
  has examples.

### Operation lines

An operation is its kind followed by what it reads, as a program writes
it:

| Line | Operation |
| --- | --- |
| `scan p(X, Y)` | `SCAN` |
| `lookup p(X, Y) key={0}` | `LOOKUP`, with its key columns |
| `member p(X, Y)` | `MEMBER` |
| `exists p(X, _) key={0}` | `EXISTS`; key shown for the partial shape only |
| `anti-member p(_, X) key={1}` | `ANTI_MEMBER`; key shown for the partial shape only |
| `filter N > 100` | `FILTER`, with its comparison |
| `assign Y = X + 1` | `ASSIGN`: the variable it binds and its expression, whichever side each was written on |
| `aggregate N = count edge(X, Y) key={0}` | `AGGREGATE`: its equality, the folded relation with columns named, and its key columns when it has any |
| `emit p(X, Y)`, `stream $q0(X)` | `EMIT`; `stream` for a query's streaming emit |

* **Keys** are `key={0,2}`, column positions from zero; under
  `-X no-index` a probe has no key, as a lookup prints as a scan.
* **Views** follow the kind when `delta` or `old`: `scan delta`,
  `lookup delta`, `member delta`, `exists delta`, and the same with `old`.
  `current`, what a member literal in a variant reads unless marked, and
  `all` are unmarked.
* **Names**: variables as the rule names them, `_` as `_`, constants as
  the text output format writes them. An operation over the base predicate
  `link$base` names it as the rule does, `lookup link$base(X1, X2) key={0}`:
  it reads the base store of `link`, whose own name denotes the derived
  relation ([Magic Sets](design-magic.md#facts-and-the-base-predicate)).
* **A dead variable prints as `_`**, in a check and a generator alike,
  since the line shows what the plan reads; the `rule` line keeps the
  author's name. `scan e(X, _)` under `rule p(X) :- e(X, _Y).` shows both
  that `_Y` is unused and that the plan does not pay for it.

### Printing expressions

Expressions, in `rule`, `filter` and `assign` lines alike, are printed
from their trees with **exactly the parentheses the tree requires**:

* an operand binding less tightly than its operator is parenthesized,
  `(A + B) * C`;
* **so is a right operand binding exactly as tightly, under every binary
  operator**, not only the non-associative ones: `2 * (3 / 2)` is 2 where
  `2 * 3 / 2` is 3, and `A + (B + C)` can overflow where `(A + B) + C`
  does not, so dropping them would show a different computation from the
  one that runs;
* a negation is `-` before its operand, parenthesized when the operand is
  an operator expression, `-(A + 1)`, or a constant other than a negative
  integer, `-(5)`, since `-5` would read back as a negative literal; a
  variable, call or negation needs none, `-A * 2`, and adjacent minus signs
  are separated by a space: `- -A`, `- -5`, `A - -1`;
* a call is written `fn(arg, ...)` and never parenthesized, being a
  primary;
* a comparison whose left operand is the symbol `not` keeps its
  parentheses, `(not) < A`, which would otherwise read back as negation.

So **parsing the printed text gives back the printed tree**, and two texts
meaning one tree print alike, so golden plans do not depend on redundant
parentheses. Each parenthesis and each negation's minus belongs to a
distinct node on the path from the root, so no token nests deeper than the
tree, and a tree the parser accepted prints as text it accepts.

### Safe to print

**Every line is printable ASCII.** A string constant can hold any byte, so
it is written as a literal with backslash and double quote escaped as `\\`
and `\"` and every byte outside printable ASCII as `\xHH`: `raw(X,
"\x1b[2J\x7f\xff\x00z")`. No program can send a terminal escape sequence
through its plan, and a callback need not escape anything.

**A line is built in a fixed buffer, never allocated**, because explaining
a plan must not be able to fail the evaluation it describes. A line longer
than 1023 bytes is cut and ends in `...`, so the cut is visible; only a
rule of hundreds of literals or a constant of hundreds of bytes reaches it.

## Failure paths and limits

**Planning can fail after work.** Planning a component can run out of
memory, or reach the memory limit while building an index, after earlier
components have derived their tuples. The evaluation fails as a whole all
the same: nothing is answered, and the next evaluation discards the stale
relations and starts over. Planning per component rather than all up front
changes when a failure is noticed, not what it leaves behind; a limit
firing during an index build fires at the total a derivation would have
reached a few tuples on, and says so, since the message names planning.

### What planning charges

The arena holding an evaluation's plans, work recipes and access
requirements, and the arenas of a query's rewrite, are charged to the memory
limit with relation and index storage, because they hold what an operation
multiplies out of the program: plans whose
number and length each follow a body's length, and a rule copy per
adornment. The arenas holding the program and a query as the caller wrote
them are not, because the caller bounds them and a query may outlive its
context ([memory](design-memory.md)). A charged arena checks the account
before taking each chunk, against the whole chunk, so a limit prevents the
allocation rather than reporting it after. The check is paid once per
chunk, not per allocation, so the limit can fire up to one chunk before the
total would have crossed it. Facts and seeds are not planned,
so a fact counts once, as its tuple. **A limit on memory is therefore a
limit on planning time**, and no limit on body length is needed.

### How a refusal is reported

A refused arena fails exactly as with an exhausted allocator: `NULL` and
the arena's sticky failure flag. Only the message differs, and the
difference lives in one place, the single function every allocation failure
already reports through. **Every operation states its activity as it
begins**, as an argument of the one function that begins an operation, so
none can begin without saying what it does; an evaluation also records
each phase it reaches. The account records that the limit refused, and the
reporting function, finding the record, reports the limit instead of
exhaustion and consumes it:

```text
relic: error: limit exceeded: memory=N while planning p/2
relic: error: limit exceeded: memory=N while planning the query
relic: error: limit exceeded: memory=N while rewriting the query
```

No reporting site needs to know about the limit, since the account and the
reporting function are the record's only parties, and an operation clears
the record as it begins, so a stale record cannot be blamed on a later
operation. A rewrite reaching the memory limit fails the query rather than
falling back to whole-program evaluation: the rewrite's own limit measures
whether it is worth having, while the memory limit is the caller's ceiling
on accounted storage and evaluation memory, not on the process.

## Determinism

* the order: integer estimates from exact counts and fixed constants, ties
  broken by body position, ready tests in written order;
* the operations: the column classification against the bound set and the
  dead mask, which depends only on the rule;
* indexes: keyed by column set, named by position;
* each round's plan: integer execution and acquisition estimates from fixed
  views and access paths, with a fixed tie rule between plans compiled once
  per component and prepared before execution;
* query names: compilation order;
* `-e`: a function of the plans; nothing in a plan is ordered by a pointer,
  a hash or a domain identifier;
* a memory limit: fires at the same chunk allocation on every run on one
  platform, but chunk headers differ between platforms, so the point may
  too.

So two runs over the same input produce the same plans, the same
derivation order and byte-identical `-e` output
([architecture](architecture.md#determinism)).

## Alternatives not taken

* **A bytecode, a runtime bound mask, or recursion in plans.** An array of
  operations whose order is the join's nesting, with binding decided
  statically, is direct, debuggable and fast enough.
* **A join-order search.** Greedy ordering suits Datalog bodies and is
  explainable; a search would make plans harder to predict for little gain.
* **Replanning during a fixed point.** It would chase changing
  cardinalities at a cost hard to bound and make `-e` depend on how many
  rounds ran. A bounded comparison of precompiled alternatives adapts to
  the round's statistics without rebuilding plans. Indexes needed only by
  an alternative are built once, when that alternative is first selected.
* **Caching plans across evaluations.** Plans are cheap to build, and a
  cached one holding relation pointers would need invalidating by whatever
  discards derived state; a plan also depends on the complete relations
  before it, so it can exist only once they do.
* **Additional histograms or per-round estimates in `-e`.** Ordinary
  ordering uses column counts, and the recursive selector uses the joint
  key counts indexes already maintain. Data scans and histograms are not
  part of round selection. Explain names the alternatives and selection
  rule without making its text depend on iteration count.
* **A generator that stops after its first match.** A resumed generator
  that matched must reject while one paused mid-walk without matching must
  continue, so a `once` flag needs a matched-already bit surviving an
  interrupt pause, and the loop would carry a generator that is sometimes a
  test. The probe answers in one read and is a test by construction.
* **Order-dependent liveness.** Marking a binding dead when nothing placed
  after it reads it computes, from the order, exactly the variables
  mentioned once, and would make the order's bound set differ from the
  plan's.
* **Projecting a generator's dead columns away.** `s(X) :- r(X, _)` binds
  `X` once per tuple, duplicates included; removing them needs a hash of
  bindings per generator to save a cost linear in the relation, while the
  exponential cost came from later literals, which are checks.
* **Evaluating tests while estimating, or folding `MEMBER` into `EXISTS`.**
  The recursive work estimate charges for a check but does not execute it;
  expression and aggregate failures belong to the selected plan's execution.
  `MEMBER` retains its canonical-index path and vocabulary. Ordinary greedy
  ranking still uses the literal's bound columns and cardinality estimate.
* **A limit on body length, or a default budget on plans.** A long body
  costs what its plans hold, which the memory limit bounds. Plans and
  variants are the evaluation itself, not an optimization with a fallback,
  so a budget refusing a plan would refuse the program; planning memory is
  unlimited by default because the data is.
* **Rescoring only touched literals, or sharing rankings across variants.**
  The first drops a constant factor over few passes, the second saves
  little and ties variants' plans together; the simple pass is the one a
  reader can check against the greedy rule.
