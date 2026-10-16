# Aggregates

This document covers how relic evaluates an aggregate literal,
`N = count { body }` and its `sum`, `min` and `max` siblings: how the body
becomes a relation, what that relation is called, how the statement holding it
is checked and published, which bodies need no relation of their own, how the
planner and executor fold a group, and what queries, the Magic rewrite and a
shared round do with one. It is an extension of the pipeline that spans the
rule compiler, the planner and the executor. What an aggregate means -- the
set reading of a group, shared and local variables, the four functions, empty
groups, mixed types, and why a sum's overflow depends only on the group's
rows -- is stated in [language.md](language.md#aggregates) and not repeated
here.

Related: [language](language.md#aggregates),
[safety](language.md#safety), [stratification](language.md#stratification),
[architecture](architecture.md#aggregates),
[expressions](design-expressions.md), [stratify](design-stratify.md),
[planning](design-planning.md), [evaluation](design-eval.md),
[Magic Sets](design-magic.md), [parallel evaluation](design-parallel.md),
[front end](design-frontend.md).

A [full grouped-count query over a projected base](design-grouped-count.md)
can consume its producer into a private collector and publish just the
answer. The compiler retains ordinary rules for every predicate; this
document describes their materialization and fold behavior.

## Model

Call the rule holding an aggregate literal, minus that literal's braces,
the **outer rule**. A variable whose every occurrence lies inside one
aggregate's braces is **local** to it; any other variable that occurs inside
is **shared**.

```prolog
dir_size(Dir, S) :- dir(Dir), S = sum(Size) { file(F, Dir), size(F, Size) }.
```

`Dir` is shared; `F` and `Size` are local; `S` is the result.

The body is compiled into a **group rule**, an ordinary rule of the program
whose head is a generated **group predicate** over the shared variables and
then the local named ones:

```prolog
agg$dir_size$2$0$2(Dir, F, Size) :- file(F, Dir), size(F, Size).
```

The aggregate literal in the outer rule becomes a **fold**: for each binding
of the shared variables, walk the group relation's tuples whose key columns
hold that binding, fold the aggregated column, and bind or compare the
result. The data flow is:

```text
statement  --classify occurrences-->  shared / local per aggregate
           --compile-->  outer rule + one group rule per aggregate
           --check, then publish all at once-->  rule table
stratify:  outer rule's head  --aggregate edge (strict)-->  group predicate
evaluate:  group relation complete in a lower stratum
           outer plan: ... -> AGGREGATE (fold key) -> ... -> EMIT
```

A body that is already a relation -- one atom of distinct variables -- gets
no group rule; the fold reads the atom's relation directly
([Folding in place](#folding-in-place)).

## Invariants

* **The group relation is the group, by construction.** Under any binding
  of the shared columns, its tuples with that key are exactly the rows the
  body denotes: distinct because a relation is a set, with no column for
  `_` because a wildcard has no slot. The fold needs nothing else.
* **A fold reads a complete relation.** The edge from a reader to the
  predicate it folds is strict, like a negative edge, so the relation is
  finished before any rule folding it runs, and it never changes while
  folded.
* **A statement is published whole or not at all.** Its group rules and its
  outer rule are appended together after every check passes, or none are.
* **Slot c is column c.** In the body an aggregate literal carries, slot
  *c* is column *c* of the relation the fold reads, for a group rule and a
  body folded in place alike.
* **Key slots are listed in column order.** The fold's key names, for each
  key column in increasing column order, the outer slot it is bound from.
* **A fold's value is independent of row order.** `count`, `min` and `max`
  trivially; `sum` because it is exact
  ([The exact sum](#the-exact-sum)). So nothing about the answer or the
  failure depends on the order a strategy inserted the group's tuples.
* **A fold writes only executor-local state.** It reads a complete relation
  and binds a frame slot; it interns nothing and allocates nothing.

## The group rule

**The body is a rule.** Compiling the body to a rule is the architecture's
"transform before complicating the evaluator". The alternative -- run the
body afresh for each binding of the outer rule with the shared slots
pre-bound -- would give the executor a nested plan, a scratch set to make
the local bindings distinct, memory growing with each group that needs an
account of its own, and a re-evaluation per binding; the naive evaluator,
the Magic rewrite and a shared round, one whose work is divided among
threads ([parallel evaluation](design-parallel.md#model)), would each need a
case for it. A
group rule gets all of that from machinery that exists: duplicates collapse
on insertion, the tuple and memory limits count the relation, the schedule
orders it, explain shows it, and a shared round divides its pass. The cost
is storing the body's relation once, which the limits bound as they bound
any relation.

The head's columns are the shared variables in the outer rule's slot order,
then the local named variables in order of first occurrence in the body. So
a group relation's key columns are its leading ones.

### Names

A group predicate is named from positions, never from a counter, so the same
program always gives the same names:

* `agg$<pred>$<arity>$<rule>$<position>` for an aggregate in a program
  rule: the outer rule's head name and arity, the rule's position from zero
  among the rules deriving that head, and the literal's position from one
  in the body;
* `agg$$q0$<position>` for an aggregate in a query.

**The arity is in the name** because a predicate is a name and an arity:
`p/1` and `p/2` are unrelated predicates whose first rules may each hold an
aggregate at the same position over bodies of the same width, and a name
without the arity would give the two groups one relation. Rules are only
appended, so a position never shifts.

**A query's group predicate is named after `$q0` whatever the query's
serial**, so that every query with an aggregate at one position and of one
arity reuses one predicate table entry rather than growing the table per
query; one of another arity gets another predicate of the same name. The
entry is all that is shared: the relation is emptied when each run ends
([Queries](#queries)), and which group rule it stands for is a fact about
the literal that reads it, never about the entry.

The names hold `$`, so no program can name, read or write a group
predicate ([naming](architecture.md#naming)), and the
undefined-predicate warning never mentions one. A group predicate is marked
derived when its rule is published, has no base store since nothing can
state a fact for it, and is marked as a group predicate when named, which
is how stratification diagnostics recognise it (see
[design-stratify.md](design-stratify.md)).

### Classification and safety

Whether a variable is local depends on the whole statement: a later literal
can share a variable an earlier body mentions. So a statement holding an
aggregate is read twice. The first pass records each named occurrence with
the region it lies in -- the outer rule or one aggregate's braces -- sorts
them by spelling, and folds each spelling to one home; the second pass
compiles with that classification in hand. This is the one place the rule
compiler's single pass becomes two, for the same reason the safety check is
a closure: the answer depends on text that comes later. The statement's
length bounds both passes, and classification is O(n log n) in its
occurrences.

Local variables take no slot of the outer rule, so several aggregates over
wide bodies do not push the outer rule towards its variable limit. The
result variable and the shared ones are the outer rule's.

The safety rule is stated once, in [language.md](language.md#safety). It is
checked by computations that exist, applied to two rules:

* **The group rule is checked as an ordinary rule.** Its head names every
  shared and every local named variable, so each must be bound by a
  positive literal or an assignment of the body. This is where a shared
  variable must be bound **inside** the braces as well as outside, and the
  diagnostic says so:

  ```text
  scores.dl:3:56: error: variable T in the aggregate of rule above/1 is not bound by a positive literal or an assignment of the aggregate body
  scores.dl:3:56: note: a variable bound outside an aggregate must also be bound inside it
  ```

  The group relation would otherwise need a column its body cannot fill.
  Supplying the outer bindings through a prefix relation, as the Magic
  rewrite's supplementary predicates do, is correct and general, but costs
  a relation per aggregate or an outer prefix evaluated twice, for a rule
  shape the writer can express by naming, inside the braces, the relation
  that binds the variable.
* **The outer rule is checked by the ordinary closure, with the aggregate
  as an assignment of its result.** The shared slots are required, as a
  negated literal's are; the result slot becomes bound once every shared
  slot is. A result that also occurs inside its own braces is therefore
  shared, so it is unsafe unless another literal binds it, in which case
  the aggregate tests that binding: `p(N) :- q(N), N = count { r(N) }`
  holds for each `N` of `q` that counts its own `r(N)` rows.

No singleton warning comes from inside a group body: every local named
variable is a head column of the group rule, so it occurs at least twice in
the rule the body compiles to. In `count { edge(X, Y) }`, `Y` is not a typo
but the column that makes rows distinct. A result variable the outer rule
never reads is a singleton of the outer rule and draws the warning.

A group predicate is a predicate, so it has at most the arity limit's
columns; a body whose shared and local named variables together exceed it
is rejected at the occurrence that would exceed it.

### Publishing a statement

A statement holding aggregates compiles to one outer rule and one group rule
per aggregate that needs one. **Nothing is appended until the whole
statement has passed.** A group rule published before its outer rule was
found unsafe would derive a relation nothing reads, and a load failing
between two appends would leave half a statement for a retry to duplicate.
So every rule is built into the arena and checked; then room for all of
them is reserved in the rule table at once -- the one step that can fail,
and a failure leaves the table as it was -- and they are placed, group rules
first and the outer rule last. A rejected statement appends nothing; its
group predicates, already interned, are referenced by no rule.

A program statement's group rules are program rules: they count toward the
program's rule count, which decides whether the derived relations are current
([currency](design-facts.md#currency)), sit in the dependency graph and the
schedule, and are evaluated with the program. A query's group rules are the
query's ([Queries](#queries)).

## Folding in place

The commonest body is one atom:

```prolog
outdeg(X, N) :- node(X), N = count { edge(X, Y) }.
total(N) :- N = count { valueAlias(A, B) }.
```

Its group rule would copy `edge` column for column into a second relation
and then index the copy. The copy is exact and useless: when every argument
of the one atom is a distinct named variable, the body denotes the atom's
own relation, so the group under a binding of the shared variables is the
set of the atom's tuples holding that binding in the columns the shared
variables stand in.

**Such a body is folded in place.** A body qualifies when it is exactly one
positive literal whose arguments are pairwise distinct named variables --
no constant, no `_`, no repeated variable, nothing beside it -- and, for
`sum`, `min` and `max`, the aggregated variable is one of them. A nullary
atom qualifies. For it the compiler generates no group predicate and no
group rule. The literal folds the atom's predicate; its **key columns** are
the atom's columns holding shared variables, each bound from that
variable's outer slot; and its aggregated column is the column of the
aggregated variable:

```text
N = count { edge(X, Y) }        X shared:   fold edge, key column 0
N = count { edge(Y, X) }        X shared:   fold edge, key column 1
S = sum(W) { weight(X, Y, W) }  X, Y:       fold weight, keys 0 and 1, column 2
N = count { valueAlias(A, B) }  none:       fold valueAlias whole
```

The key is then a set of columns rather than a prefix, which the planner and
executor already handle: an index is keyed on a set of columns, and a probe
is a tuple with only the key columns read.

**Key columns are listed in column order, not in the outer rule's slot
order.** For a group relation the two coincide, because its leading columns
are the shared variables in slot order. For a folded atom they need not:
in `S = sum(W) { w(Y, X, W) }` with `X` in slot 0 and `Y` in slot 1, key
columns 0 and 1 are bound from slots 1 and 0. Listing the key in slot order
would build the probe with the values swapped and fold the wrong groups,
silently -- no error, just wrong sums. The literal therefore stores, in
increasing column order, the slot each key column comes from.

**The literal keeps a description of its body.** Explain prints the braces
from it, the Magic walk visits what it reads, and a note names the
aggregated variable from it. For a body folded in place, that description
is a one-literal rule with **no head**, never published to a rule table,
never evaluated, in no graph; its variables are numbered in the order the
atom's arguments are written, so its slot *c* is column *c* of the relation
folded -- the same invariant a group rule's numbering gives. A headless
rule cannot be mistaken for a derivation. The identity rule
`edge(X, Y) :- edge(X, Y)` would have made "the predicate a literal folds"
always its body's head, one field fewer, but it says "this derives `edge`"
to every reader that does not know the convention.

**The literal stores the predicate it folds and its key mask**, though both
follow from the body, so that every reader -- the dependency graph, the
planner, the Magic walk, the structural hash -- treats the two forms
identically, without asking which one it holds. Only the compiler that
makes them and the printer that shows them distinguish them. The literal's
body is a copy in the arena that holds the literal, not a position in a
rule table: the table grows and moves, a query's group rules are held by
the query, and a copy the Magic rewrite makes must reach the body the same
way (see `lib/program.h`).

**The answers do not change.** The group rule's relation is the atom's under
a permutation of columns, every function gives the same value over the same
rows, and stratification places the reader in the same stratum: one strict
edge to the atom puts it where a strict edge to a group predicate, whose
one positive edge led to the atom, did. What surrounds the answer may
differ: explain shows the fold over the atom and no group rule; the tuple
count has no copy, so a tuple limit a copy would have reached is not; a
memory limit refusing the fold's index names the indexed relation rather
than an aggregate; the cycle reported for an unstratified program may be a
different, equally valid one; and two structurally equal folds of two
rules, which distinct group predicates kept apart, merge in the rewrite's
deduplication.

**What does not qualify stays a rule.** `count { edge(X, _) }` is a
projection, `count { edge(X, a) }` and `count { edge(X, X) }` selections;
each denotes a different relation from the atom's and needs the rule that
computes it. Folding a selection through an index that includes the
constant's column, or an extreme over a projection, is possible; each is a
second way to build a key, for shapes the degree counts, totals and extremes
that motivate folding in place do not take.

**`RELIC_F_GROUP_RULES`** (`-X group-rules`) compiles every body to a group
rule. It is a strategy in the testing sense (see
[design-testing.md](design-testing.md)): its answers must equal the
default's, and the generated programs produce single-atom bodies often
enough that every fold's column map is checked against the copy it
replaces. It also serves the random tester's derivation oracle, which
counts derivations per group body: rather than giving the tester its own
copy of the compiler's "is this body a relation" test, which would drift,
the oracle runs under the flag, where every body derives.

## Stratification

The reader of an aggregate depends on the predicate it folds -- the group
predicate, or the atom's predicate when folded in place -- through an
**aggregate edge**, which the analysis treats as a negative edge in every
respect: it may not lie inside a component, and it places its source at
least one stratum above its target. So a relation is complete before it is
folded, and an aggregate in recursion is rejected statically, with the
diagnostic recursion through negation gets. A group predicate is derived by
one rule and read positively by nothing, so in a stratified program it is a
component of its own, never recursive, evaluated in one pass.

The unstratified diagnostic names the aggregate and never a generated
predicate: an aggregate edge to a group predicate and the group rule's edge
that continues the cycle are reported as one note pointing inside the
braces. How the closing edge and the cycle are chosen is
[design-stratify.md](design-stratify.md)'s.

## Planning

An aggregate literal becomes one **AGGREGATE** operation. It is placed with
the tests, at the earliest point where every shared slot is bound, and
placing it can make further tests placeable. If the result slot is still
free there, the operation **binds** it; if an earlier generator, assignment
or fold bound it, the operation **compares** the fold's value with the
binding, as an equality over two bound sides is a filter. Which of the two
happens is a fact about the plan; the answer is the same either way, by
the language's argument that a schedule cannot change what a successful run
derives. Earliest is not always cheapest -- a fold read only by the head is
done before later literals have rejected bindings -- and why the planner
does not try to place it later is in [design-planning.md](design-planning.md).

The relation folded is complete, so the operation reads it through the
whole-relation view, never through a delta: a fold has no semi-naive
variant of its own, and in another literal's delta variant it folds the same
relation every round, as an anti-member tests the same relation. The rows
of a group are reached through an access chosen by the planner and
written into the operation so the executor has no branch on the strategy:

* **no key column**: read the whole-view cardinality for `count`; other
  functions walk the whole relation, which is then the group;
* **some key columns**: walk the chain of the index keyed on exactly those
  columns, built when the first plan that needs it is made and kept with
  the relation's other indexes
  (under `RELIC_F_NO_INDEX`, walk the whole relation and match the key
  columns of each tuple, as a generator with bound columns is scanned
  there, so the reference strategy has no index the optimized one lacks);
* **every column a key column**: one probe of the full-tuple index, since
  the group is then the one tuple the key names or nothing.

The result slot's kind follows from the function: `count` and `sum` bind a
**number slot**, their result being an integer the executor computed;
`min` and `max` bind a **domain slot**, their result being a value the group
holds, with an identifier already (see
[design-expressions.md](design-expressions.md#slots-and-cells)). A later
literal keyed on the result is looked up through the slot's frame entry.

## Execution

An unkeyed count reads the complete source view's tuple count. It visits
no row, allocates nothing and adds no tuple-read counter. Other functions
and keyed counts use the row-fold accesses below.

A fold builds its key from the key slots' frame entries, laid out as a tuple
of the relation with only the key columns read, reaches the group's rows as
its access says, and folds each row's aggregated column:

```text
count:      count the rows
sum:        the column must decode to an integer, else no value;
            add it to the exact sum
min, max:   the first row fixes the type and the extreme; a row of
            another type means no value; otherwise keep the extreme
```

At the end `count` gives the row count; `sum` asks once whether the total
fits and either gives it or records an overflow error; `min` and `max` give
the extreme's identifier, or have no value for an empty group, which is
the ordinary meaning and draws no note. A value outside the function's
domain records the mixed-type note and rejects the binding. The note and
the error travel through the executor's two records like an expression's
(see [how notes and errors cross the executor's
boundary](design-eval.md#notes-and-errors-cross-the-executors-boundary));
a note raised inside a group rule is reported as about the statement the
aggregate was written in.

When the operation compares rather than binds, the fold's value is a cell --
an integer for `count` and `sum`, an identifier for `min` and `max` -- and
it is compared with the cell the bound slot gives by the slot's kind, by the
ordinary rule for comparing cells. A count of two equals a number slot
holding two whether or not the integer table holds the integer; the frame
entry, which may be zero, is not what is compared.

When the operation binds a number slot, the integer goes into the frame's
number array and the frame entry is set by the one read-only probe every
computed integer gets; a count the table lacks reaches a head tuple as a
pending integer and is interned when the batch is stored (see
[design-values.md](design-values.md)).

The accumulator is a fixed-size local of the fold, so a fold allocates
nothing. For the same reason a fold cannot pause midway: it charges the rows
it reads to the executor's work quantum and runs to the end, and the
executor pauses after it. The walk holds no tuple pointer beyond the row it
is folding, and nothing inserts into a complete relation in any case.

The cost is one tuple read and one decode per row of the group, per binding
that reaches the operation. A rule that binds the same key many times folds
the same group many times.

### The exact sum

`sum` accumulates in a 128-bit two's complement integer held in two unsigned
64-bit words, without `__int128`, which `configure` would have to test for
and some targets lack. Every operation is on unsigned words, whose
wrap-around is defined, so there is no signed overflow anywhere in the fold
(`lib/arith.h`).

**Why 128 bits suffice.** A tuple identifier is 32 bits, so a relation, and
so a group, has fewer than 2^32 rows. Each addend lies within 2^63 in
magnitude. The total of fewer than 2^32 such addends therefore lies within
2^95 in magnitude, well inside what 128 bits hold, and every partial sum
along the way obeys the same bound. The accumulator never wraps, so its
final value is the exact total over the integers whatever order the rows
were visited in. Whether that total fits `int64_t` -- whether the high word
is the sign extension of the low one -- is asked once, at the end.

That is what makes overflow a function of the group's set of rows. Adding
in `int64_t` with a check per step would overflow on `INT64_MAX, 1, -1` and
not on `1, -1, INT64_MAX`, and since a group's visit order is its insertion
order, which differs between strategies, whether a program failed would
depend on the strategy in a way no equivalence could state.

### Parallel rounds

A fold reads a complete relation and writes only its own frame, so it needs
nothing new from a shared round: a number slot in the frame, a pending
integer in a candidate, the measure and rewrite before the merge, and an
overflow that abandons the job and reruns the round serially are all the
expression machinery's (see [design-parallel.md](design-parallel.md)). A
group rule is a one-pass component, so its pass is shared among a round's
threads as any one-pass component's is.

## Queries

A query may hold aggregates. Its output columns are the named variables of
its outer rule, so local variables are not among them. A query's group rules
**belong to the query**, not to the program's rule table: appending them to
the program would make every prepared query with an aggregate a change to
the program, discarding what was derived and forcing a materialization the
query was prepared to avoid. They are compiled with the query's own rule,
held by the query, and run by it. Answering a query is one sequence:

1. discard what a stale evaluation discards;
2. evaluate the program's part: the whole program's fixed point, or, when
   the query is rewritten, the part the rewrite reads completely;
3. **run each of the query's group rules once**, in body order, into its
   group relation (a fold in place has none to run);
4. evaluate the rewrite's generated rules, when the query is rewritten;
5. run the query's plan, and deliver its answer.

Step 3 is one pass per group rule, since a group rule is never recursive and
everything it reads is complete after step 2.

**Step 3 must precede step 4 because the rewrite may copy the aggregate
literal.** The rewrite appends the query's literals to the bodies of the
magic and supplementary rules it drafts, so

```prolog
N = count { v(X), w(X) }, p(N)?
```

drafts a seed such as `magic$p$b(N) :- N = count { v(X), w(X) }.`, a
generated rule that folds the query's group relation. Had the group been
filled after the generated rules ran, the seed would have folded an empty
relation, demanded `p(0)`, and found nothing where the whole program
answers. A rewrite abandoned over its limit falls back to the whole
program's fixed point in step 2 and has no step 4, and the order holds.

**The group relations are emptied when the run ends**, with the answer
relation, so outside a run a query holds nothing derived and a second run
derives the same rows in the same order. What a run may leave behind is an
index: a fold in place over a program relation builds the index on its key
columns in that relation, which stays with the relation and is charged to
the memory limit, as a lookup's index is, until the relation is discarded.

A query whose plan holds a fold is gathered rather than streamed: `sum` can
fail on any binding, `count` can put a pending integer in a row, and `min`
and `max` are gathered with them so that whether an aggregate query streams
does not depend on which function it uses (see
[design-eval.md](design-eval.md)).

## Magic Sets

The rewrite (see [design-magic.md](design-magic.md)) treats an aggregate
literal as it treats a negated one, for the same reason: a fold needs the
whole group, and reading such a relation completely in its lower stratum
keeps the rewrite simple.

* **What a fold reads is read completely**, and so is everything its body
  reads: the walk that classifies predicates adds the folded predicate and
  every predicate the literal's compiled body names. For a body folded in
  place the two are one predicate, and the walk needs no case to tell them
  apart. A query's group rule is in no component, since the query runs it
  itself, so what its body reads is marked directly.
* **The literal is appended as it is.** In the planner's order it is a test
  placed where its shared slots are bound, and it binds its result there,
  so a call after it whose argument is the result is adorned bound. The
  rewrite copies the literal with its shared and result slots renumbered and
  its folded predicate as it is, never adorns the folded predicate, and
  hashes and compares the literal structurally -- function, folded
  predicate, key columns, aggregated column, slots -- when deduplicating
  generated rules.
* **A guard catches the impossible case.** A fold in a generated rule over a
  predicate not read completely would fold an empty or a demanded part of a
  relation; the rewrite checks for it and fails the query with an internal
  error rather than answer wrongly.
* **An aggregate literal's size is one.** It holds no expression tree and
  its body is never copied, so the rewrite's limit charges it one literal.

## Explain

Explain prints an aggregate literal as written, the braces from its compiled
body; a group rule as the generated rule it is, under its own component for
a program rule or under the query's heading for a query; and the fold as
the equality it stands for, the relation it folds with its columns named
from the compiled body, and its key columns:

```text
aggregate S = sum(Size) agg$dir_size$2$0$2(Dir, F, Size) key={0}
aggregate M = max(S) dir_size(D, S)
aggregate N = count edge(X, Y) key={0}
```

The first folds a group relation, the other two an atom's own relation. The
explain format as a whole is [design-planning.md](design-planning.md)'s.

## Failure paths and limits

A group body has at most the arity limit's variables, shared and local
together, and an aggregate body cannot hold an aggregate, which keeps the
parser's descent through braces to one level however the text is shaped
(see [language.md](language.md#limits)). An untrusted program gets three
tools from aggregates; each is bounded:

* **Undefined behaviour.** The sum is exact in 128 bits and checked once;
  `count` cannot overflow, since a group has fewer than 2^32 rows; the
  parser recurses through braces at most once.
* **Unbounded memory.** A group relation is a relation, charged to the tuple
  and memory limits before it grows. The limit message names it as the
  reader wrote it, as an aggregate of the predicate whose rule holds it --
  `limit exceeded: tuples=20 while deriving an aggregate of pairs/1` -- or
  of the query, since the generated name says nothing the reader can act on.
  A fold in place stores nothing but its index, charged as every index is.
  A fold itself allocates nothing.
* **Unbounded time.** A fold is linear in its group, and a group is a relation
  the limits bound. What the limits do not bound is the work of deriving a
  group. By default a literal that binds nothing the rule reads is an
  existence check
  ([planning](design-planning.md#dead-variables-and-existence-checks)), so
  `count { b(_), b(_), b(_) }` costs a probe per literal; under `-X no-exists`
  it joins `b` with itself three times and collapses every combination into
  one row with no columns, so the relation stays at one tuple while the work
  grows as the size of `b` cubed. That is the position every join is in,
  whether or not an aggregate reads it. The limits bound what is stored, not
  what is computed; aggregates add no way to spend time that a rule without
  one lacks. A caller that needs a time bound interrupts the evaluation.

Nothing a fold computes reaches a caller unformatted: a count or a sum is an
integer, and a `min` or `max` is a value the group held. No diagnostic
quotes a value from a group; the generated names a message may quote are
built from positions and a predicate name the program wrote.

## Determinism

* Which variables are local, and the group rule's columns, name and position
  in the rule table, are functions of the statement's text and the load
  order. Whether a body is folded in place is a function of its text and of
  `RELIC_F_GROUP_RULES`, never of what the relations hold.
* A group relation's tuples are inserted in the order the strategy derives
  them, and every function's value, and whether a sum fails, is independent
  of that order. So answers and failures are the same under every strategy
  that reaches the group and at every thread count.
* Where each fold is placed is a function of the source and the strategy.
* A count is interned in serial derivation order at its first stored
  occurrence, like any computed integer, so identifiers and value counts are
  the same at every thread count.
* An overflow names the first overflowing sum in serial order, because the
  serial evaluator always reports it.
* The aggregate notes name no type and no row, only what is true of the
  whole group, so the row a fold happens to meet first decides nothing a
  reader can see; which rule's note comes first may depend on the strategy.

## Alternatives not taken

The alternatives argued in place above -- evaluating the body per outer
binding, binding a shared variable only outside the braces, the identity
rule as a folded body's description, folding selections and projections in
place -- are not repeated here.

* **A relation of folded results.** A second generated rule folding each
  group once, read by the outer rule with a default for an empty group,
  would make a repeated fold one lookup instead of one walk, at the price of
  a second relation, a generator over groups and a lookup with a default.
  A fold costs what its group holds, which the limits bound, while the
  second relation would cost every program with an aggregate, repeated
  folds or not.
* **Placing a fold late.** The planner has no estimate of a group's size to
  justify it.
* **Demand through an aggregate.** A folded predicate is read completely,
  as a negated one is, so that the rewritten program stays stratified
  ([Magic Sets](design-magic.md#what-a-query-reads-completely)).
* **Streaming a query with `min` or `max`.** A rule that streamed one
  function and gathered three would be a list to maintain, for queries that
  are rarely large.
