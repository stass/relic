# Evaluation

This document covers how relic runs what the planner built: the executor
that runs one plan as a join, the naive and semi-naive fixed points that
bring each component of the program's schedule to its fixed point, when the
derived relations count as the program's fixed point and why everything is
discarded otherwise, how a query's answer reaches the caller, and how an
evaluation is interrupted. It is the pipeline stage after
[planning](design-planning.md), and reads and writes the
[relation store](design-storage.md).
Running a round on several threads belongs to
[parallel evaluation](design-parallel.md) and appears here only where it
constrains the serial design.

Related: [architecture: evaluation](architecture.md#evaluation),
[meaning of a program](language.md#meaning-of-a-program),
[stratification](design-stratify.md), [planning](design-planning.md),
[storage](design-storage.md), [facts](design-facts.md),
[expressions](design-expressions.md), [aggregates](design-aggregates.md),
[Magic Sets](design-magic.md), [parallel evaluation](design-parallel.md),
[library interface](design-api.md)

## Model

The evaluator operates on relations and tuples, not on a Prolog-style
stack. A checked program comes with a **schedule**: its strata in order and,
within a stratum, the strongly connected components of the dependency
graph, each after everything it depends on
([stratification](design-stratify.md)). A component's predicates are its
**members**; every other predicate its rules read belongs to a component
already evaluated and is **complete**.

```text
schedule -> component -> plans -> executor -> batch -> taker
                                                  |-> insert into a relation
                                                  |-> deliver as rows
                                                  `-> gather as candidates
```

* A component is planned when it starts, once its external inputs are
  complete, so the planner can measure those relations. Ordinary and
  delta-first plans acquire their indexes then; an alternative acquires
  extra indexes at first selection, before that round executes. Plans are
  never rebuilt inside a fixed point.
* The **executor** runs one plan, an array of operations ending in `EMIT`
  (`lib/plan.h`). It reads the context and the relations, writes only its
  **scratch**, derives head tuples into a **batch**, and returns when the
  batch is full, when a quantum of work is done, when it has recorded an
  error, or when the plan ends.
* The **taker** is the evaluation code that called it. It takes the batch,
  inserting it into its own destination relation, delivering it to a
  query's caller as rows, or, in a **shared round** (a round whose work is
  divided among threads, [parallel evaluation](design-parallel.md#model)),
  screening it into candidates; then it resumes the executor.
* A **round** is a sequence of plans each of whose views is fixed when the
  plan starts. A non-recursive component needs one round; a recursive one
  runs rounds until one inserts nothing.

A program's facts live in **base stores** ([facts](design-facts.md)), which
evaluation reads and never writes. A member's derived relation is
**seeded** from its base store when its component starts, and otherwise
holds only what its rules derived.

## Invariants

* **Append-only within a fixed point.** No relation is compacted,
  renumbered or loses a tuple while an evaluation runs. The store offers no
  operation that could, so this is a property of its interface, not a rule
  callers obey. Every delta and view is a range of tuple identifiers and
  means something only because of it: a range taken before a compaction or
  deletion would silently name different tuples afterwards.
* **No view reaches past the end its relation had when the plan started,**
  so what a plan's batches insert is invisible to it by construction.
* **The executor is a reader.** It reads through `lib/frozen.h` and writes
  only its scratch and, benignly, an index key's frontier
  ([storage](design-storage.md)). It does not insert, intern or report.
* **Zero in the frame means absent**: no relation holds a column of zero.
* **The relations are current exactly when a recorded pair of counts matches
  the program**; otherwise they are discarded before the next evaluation.
* **No row is delivered before everything that can fail has succeeded,**
  except for a caller's interrupt.
* **Every refusal a program can see is made by serial code on the calling
  thread**; a worker of a shared round abandons and never reports.

## The executor

### One loop and an explicit level

The join a plan describes is a nest of loops, one per generator. The
executor runs it with one loop and an explicit level rather than a C call
per level:

```text
level = 0, resume = false
forever:
	ask operation `level` for a match
	    (a fresh start if not resume, else its next match)
	matched:	level = level + 1, resume = false
	exhausted:	if level == 0, stop
			level = level - 1, resume = true
```

Descending means the operation at `level` produced a binding and the next
one starts fresh against it; backtracking asks the previous operation for
its next match. `EMIT` never reports a match: it adds the head tuple to the
batch and says there is nothing below it, so the level never passes the end
of the plan and the loop needs no end-of-plan case. A rule of ten thousand
literals uses no more C stack than a rule of one. Because the loop's own
state is only the pair (level, resume), and everything else is in the
scratch, the executor can stop anywhere and resume exactly where it stopped.

### The scratch

Everything the executor writes is in one scratch, one per worker when a
round runs on several threads (`lib/exec.h`):

* **the binding frame**, one domain identifier per slot. A slot is written
  by the operation that binds it and read only by later ones, so a new match
  overwrites the old binding and nothing is saved or restored on
  backtracking;
* **one state per operation**: its view's range, resolved when the plan
  starts, and for a generator the cursor of what it has yet to visit;
* **the batch**, where the join stood when it returned, the work counted
  since the last take, and the note and error records below;
* computed values: integers beside the frame, strings on a text stack, the
  cells expressions run on, pending head values, compiled patterns
  ([expressions](design-expressions.md)).

The executor allocates nothing plan-sized while it runs: the plan, the
operation states and the batch are allocated before it starts. The scratch
buffers sized by data -- the text stack, the pending head bytes, the regular
expression work space, the candidate buffers of a shared round -- are
admitted by the memory limit and may grow during execution; a refused growth
is an error the executor records.

### A generator never holds a tuple pointer

A `SCAN` reads a tuple by identifier, binds from it and lets go; resumed, it
fetches the next by identifier. A `LOOKUP` walks its key's chain holding
only the next identifier. No pointer into relation storage survives across
the operations below a generator or a return to the taker, because the
taker may insert into the very relation being scanned, which can move its
storage.

A whole-relation `SCAN` ends where the relation ended when the plan started,
and a `LOOKUP` stops at the first chain tuple at or past that end;
identifiers increase along a chain, so nothing before it is skipped. Tuples
inserted meanwhile are not lost: the next plan to read the relation sees
them. A plan's work is thus bounded by the relations' sizes when it started.

### Partitioning a leading scan

A deferred round can divide a plan whose first operation is a `SCAN` into
ordered pieces of that scan's fixed identifier range. Each piece starts
the ordinary executor at level zero, with only the first operation's view
narrowed. Term matching, all later views, quantum pauses and batch returns
follow the whole-plan path. An empty piece derives nothing.

A plan beginning with any other operation runs whole. Leading tests are
ordinary operations and keep their counters, notes and errors in the same
scratch as the rest of the plan. Every task starts at level zero, and
exhausting that level ends it; there is no separately prepared cursor or
lower execution boundary to preserve across returns.

### The executor does not insert

EMIT carries the head terms, checked output arity and logical predicate,
not a relation handle. Evaluation pairs that schema with its insertion,
delivery or [counted destination](design-grouped-count.md) and checks the
input arity before execution. A counted destination projects complete EMIT
rows to keys; it does not replan the producer or narrow its scratch.
Ordinary relation readers never interpret an EMIT as an input atom.

The executor returns when its batch is full and continues from the same
place when called again; its caller takes each batch before resuming it.
Everything that can fail or change the context happens in the take:
interning, inserting under the limits, reporting, calling a row callback.
So one executor serves every destination, the taker alone tells them apart,
and the executor fails only by recording that it did -- which is what lets a
worker run it over a store nothing writes.

The ordinary batch publisher owns work accounting, notes, error rejection
and pending-value publication. Set insertion and counting then consume
canonical tuples through their checked destination. Counting owns an
anonymous key relation and counts, with one reserve/apply contract for the
pair. It adds no second interning path or serial query-replay policy.
Deferred execution uses the same destination contract after its existing
pending-value resolution and ordered merge.

A completed destination also supplies its result rows. An ordinary set lends
the gathered relation's tuples. A counted destination lends a checked
projection of its keys and assigned count values, after the evaluator has
published every count through the existing pending-value operation. That
operation supports descriptor-only batches, with no tuple rewrite. Count
publication covers at most 64 groups per batch, including already assigned
counts. One delivery loop reads either completed destination without
allocation; the owner remains alive until delivery ends. Counting therefore
needs no second answer relation or separate callback protocol.

**Batching is invisible by construction.** Every view of a plan was resolved
when the plan started: a member view ends at its delta's end, the member's
end when the round began, and `all` at the relation's end when the plan
started. A batch inserted while the plan runs takes identifiers past all of
them. So what a plan reads does not depend on when, or in how many batches,
its tuples are inserted, and the batch size is a constant of performance
alone. The batch exists to overlap the cache misses of consecutive duplicate
checks: the store hashes and prefetches a whole batch's slots before
inserting any of it.

A batch is inserted in derivation order, one tuple at a time, so each tuple
takes the identifier it would have taken alone, and the counters of a
successful evaluation are those of unbatched insertion. A refusal inserts
the tuples before the refused one and discards the rest, which the failed
evaluation discards anyway. The tuple limit refuses the same tuple with the
same diagnostic, but the executor may have run up to a batch past it, so a
failed evaluation can show three differences: larger work counters, a
mixed-type note from a binding after the refused tuple (the note is
reported before the batch is inserted), and a memory limit reached up to a
batch buffer's size earlier, since the batch is charged to the evaluation
arena.

Serial insertion can screen confirmed duplicates through a
[plan-owned positive bitmap](design-serial-membership.md). Its unset bits
make no absence claim, and unknown tuples keep their order through the
ordinary batch inserter. It releases its evidence when that plan ends.

### Zero in the frame means absent

The executor never interns, because interning writes the context. A
computed value is kept beside the frame, and its frame entry is the
identifier a read-only probe of the value table finds, or zero when the
table holds none. No relation holds a column of zero, so such a slot matches
nothing, which is right for a value no relation can hold; an `EMIT` leaves a
pending marker for it that the taker interns before the batch goes anywhere.
The mechanism and why zero needs no case of its own are in
[expressions](design-expressions.md#slots-and-cells).

### Notes and errors cross the executor's boundary

Reporting calls the caller's diagnostic callback and changes the context, so
the executor only records; the taker reads each record when it takes the
batch.

* **The note** on an ordering comparison between values of different types,
  which the language defines as false
  ([comparisons](language.md#comparisons)). It is recorded only when the
  context asks for such notes (`RELIC_F_NOTE_MIXED`), at most once per
  program rule for the life of the context; a rule a Magic rewrite generates
  and every semi-naive variant count as the program rule they came from,
  and a query's rules share one mark, cleared when an answer begins. The
  note is reported before the batch's tuples.
* **The error**: an integer overflow, a zero divisor, a string too long, a
  pattern that is not a regular expression, a `sum` that does not fit, or a
  buffer growth the limit or the allocator refused. The executor records
  the location and ends the plan as though every generator were exhausted.
  The taker reports the error in the rule that computed it, discards the
  batch and fails the evaluation.

An undefined result fails the evaluation rather than dropping the binding:
skipping it would make a negation over its result true, and which bindings
are reached differs between strategies
([evaluation errors](language.md#evaluation-errors)).

In a shared round, a task whose executor records an error **abandons the
job** and the round runs again serially. The serial run meets,
in serial order, whatever the serial evaluator would meet first -- this
error, an earlier one, or a limit an earlier batch reaches -- and reports it
with the serial message. The outcome does not depend on how tasks fell
across threads, and the rerun costs a round that was failing anyway.

## Evaluating a program

A component is seeded, planned and brought to its fixed point: in one pass
when it is not recursive, otherwise by the semi-naive fixed point, or by the
naive one under `-X naive` (`RELIC_F_NAIVE`).

**Seeding.** A member's whole base store is inserted into its derived
relation, in identifier order and in batches, as an `EMIT` inserts. Each fact
counts as a derivation. A predicate of facts alone is in no component and is
read from its base store directly.

**Body-less rules.** Facts are stored, not compiled, so a rule with no body
occurs only as a seed of a Magic rewrite, such as `magic$path$bf(a).` It is
not planned: its head is all constants, and the pass inserts it when its
turn comes, counted as a one-`EMIT` plan would be.

**Negation and aggregates read complete relations.** A negated literal is a
test that holds when no matching tuple exists; an aggregate folds a group
relation. Stratification puts both relations strictly below the reading
rule, and the schedule runs every component after everything it depends on,
negatively as well as positively, so the test and the fold see a complete
relation with no check in the executor
([stratification](language.md#stratification)). Neither literal reads a
member of its own component, so neither has a variant of its own, and in
another literal's variant it reads the same complete relation every round.

### The naive fixed point

```text
for each recursive component:
	repeat:
		for every rule of the component, in program order:
			run its plan
	until a pass inserted no new tuple
```

**Termination.** Without expressions every value a head can hold was
interned before evaluation began or is an aggregate's result: a head holds
constants and variables, and a variable is bound from a tuple or by an
aggregate, which folds a complete relation once per binding of its group,
of which there are finitely many. So each relation is finite and a pass
that adds nothing is the last. A rule that computes a new value from
an old one, such as `n(Y) :- n(X), Y = X + 1`, has no such bound; there
termination is the program's responsibility, and the limits, an overflow
error or the caller's interrupt stop it ([limits](architecture.md#limits)).

**Least fixed point.** Every inserted tuple is justified by a fact or by
tuples already present, so by induction on insertion order every inserted
tuple belongs to every fixed point that contains the facts and agrees on
the complete lower strata. A component is left only when a full pass adds
nothing, so its relations are a fixed point; with the lower strata complete
it is the least one. No later component writes a predicate an earlier one
reads, so once the last component is done no rule adds anything: the
stratified model of
[the program's meaning](language.md#meaning-of-a-program).

Within a pass a rule sees what the rules before it inserted, and none of its
own derivations, since its plan reads each relation as it stood when the
plan started. That changes how many passes are needed, not what they reach,
and it is deterministic.

**The oracle.** A **base pass** runs every rule of a component once, in
program order, each through its base plan, in which every literal reads its
whole relation. The naive fixed point repeats base passes; the semi-naive
fixed point runs one as its round 0. Sharing that pass and everything below
it, a disagreement between the two isolates the fixed-point loops; why the
oracle lives in the library is in [testing](design-testing.md#the-oracle).

### The semi-naive fixed point

After round 0, a recursive rule runs only against what its recursive
literals gained in the previous round:

```text
for each recursive component:
	every member's delta = empty at the first identifier
	round 0: every body-less rule and every exit rule, in program order
	while some member gained a tuple in the last round:
		advance the deltas
		select every due variant's plan from the round-boundary statistics
		for every due variant, in planning order:
			run its selected plan
```

**A delta is a range of tuple identifiers.** The store is append-only, so
what a member gained in one round is exactly the tuples between its end
before the round and its end after it. No delta is copied or has an index of
its own; a `LOOKUP` reaches a delta's start through a per-key frontier in the
member's own index ([storage](design-storage.md)).

**Views.** Every operation over a relation reads through one of four:

| View | Range | Meaning |
| --- | --- | --- |
| `all` | start to the relation's end when the plan started | the only view of a complete relation |
| `old` | start to the delta's start | a member's tuples older than its delta |
| `delta` | the delta | what the member gained in the previous round |
| `current` | start to the delta's end | everything the member held when the round began |

The planner sets each operation's view; the evaluation resolves its range
when the plan starts. Every operation of a naive plan, a query's plan and a
non-recursive component's plan reads `all`.

**Variants.** A rule whose positive literals over members of its component
are, in body order, *m1* to *mk*, has *k* variants. In variant *i*, literal
*mi* reads `delta`, member literals before it read `old`, those after it read
`current`, and every other literal reads `all`. Views follow body order, not
plan order: the view decides which tuples a literal can match, the nesting
decides only what each binding costs, and a join's result does not depend on
its nesting. A variant retains at most two plans constructed by structural
ordering policies. Before any item of a round runs, its plans are compared
using execution and acquisition estimates from those same views and
available indexes. All choices precede preparation of missing access
paths, which finishes before any round item executes. The prepared
selected plan belongs to the round item and is also the plan a declined
shared round retries serially. Candidate construction, estimates and the
selection boundary are specified in
[recursive plan selection](design-recursive-planning.md).

An **exit rule** has no positive literal over a member, as
`path(X, Y) :- edge(X, Y)`. It has no variant, since everything it reads was
complete before the component began. A recursive rule has no base plan under
the semi-naive fixed point and runs only as its variants.

### Round 0

Round 0 runs the body-less rules and the exit rules once each, in program
order, after the members were seeded. At the start of the component every
member's delta is set empty at the first identifier, and every member that
holds tuples is recorded as having gained them, as though round 0 had
inserted them. What a member holds then is exactly what seeding put there,
because the derived relations were discarded before the evaluation and
nothing else writes a member. A recursive rule runs only as variants, and
each variant needs a tuple in its delta; a seeded fact that never entered a
delta would never take part in a recursive derivation, so the seeded tuples
must form part of the first delta. **The first delta of each member is
therefore the whole of it**, and round 1 is the first to run a variant.

### Why every derivation is made exactly once

Say a member tuple belongs to the round that inserted it, round 0 for a
seeded fact. Take any way of satisfying a rule's body, its member tuples
being *t1* to *tk* in body order; let *m* be the latest round any of them
belongs to, and *i* the first position whose tuple belongs to round *m*.

*Completeness.* Round *m* inserted a tuple, so round *m* + 1 runs. At its
start each member's delta is its round-*m* tuples, `old` its tuples of
earlier rounds, and `current` its tuples up to round *m*. Variant *i* is due
in that round: its delta member gained *ti* in round *m*, and each of its
`old` views holds some *tj*, so it is not skipped. It finds *ti* in its
delta, every *tj* with *j* < *i* in `old` (they belong to rounds before
*m*), and every *tj* with *j* > *i* in `current` (none belongs to a later
round); complete literals match in `all`. So variant *i* derives the head
from these tuples in round *m* + 1.

*Uniqueness.* Variant *j* < *i* would need *tj* in its delta, but *tj* is
older than round *m*. Variant *j* > *i* would need *ti* in `old`, but *ti*
belongs to round *m*. A round before *m* + 1 cannot see *ti*, which was not
inserted before that round began, and views end where the round began; a
round after it has deltas to which none of the tuples belongs. Within the
one run, the combination is found once: a round runs only one of a
variant's two plans, and a view lists each tuple once. All-round-0
combinations are the case *m* = 0, derived by variant 1 in round 1 from the
whole of each member.

The argument rests on two facts: the first delta of each member is the whole
of it, and no view reaches past the end its member had when the round
began. Termination and minimality are the naive fixed point's arguments. A
round derives only from the previous round's tuples, so a component may take
more rounds than the naive fixed point takes passes when its facts happen to
be in a favourable order, but never more than its longest derivation chain.

### Which variants a round runs

**A round costs what changed.** A join over an empty operand is empty, so a
variant whose delta is empty, or one of whose `old` views is, derives
nothing. A round runs, in planning order, only variants whose delta literal
reads a member that gained in the previous round and whose every `old` view
holds something; skipping the others changes only the cost. Variants are
grouped by the member their delta literal reads when the component is
planned, and the members that gained are recorded as plans insert -- every
insertion during a component's fixed point is into the head of the plan
making it, a member -- so a round looks only at the members that changed.
Without this, a recursive cycle of *n* predicates, which takes *n* rounds of
one new tuple each, would run every variant every round: quadratic. In round
1 every delta starts at the first identifier, so every `old` view is empty
and only variant 1 of each rule runs -- the *m* = 0 case of the proof; the
empty-`old` skip keeps round 1 from running later variants against empty
views.

**The `old`-view countdown.** A delta's start never moves back, so an `old`
view, once nonempty, stays nonempty. Each `old` view a variant's plans read
is counted once, read off the plans when the component is planned, and a
variant is due only when its count of empty `old` views is zero. When a
member's delta first starts past the first identifier, each such view of
that member is decremented once. A member crosses that point once, so the
decrements over the whole fixed point equal the `old` views the variants
read -- *k*(*k* - 1)/2 for a rule of *k* member literals -- paid once per
fixed point rather than once per round. Testing each candidate's `old` views
every round instead would cost that much every round.

### The round boundary

**Invariant: at the start of every round, each member's delta is exactly
what it gained in the previous round, possibly nothing, and ends at the
member's end.** The boundary keeps it by advancing, **once each**, the
members of two sets: those that gained in the round just run, and those
whose delta was nonempty in it. Each is advanced to the tuples past its
delta's end: what it gained, or an empty range at its end. Advancing a member
twice would empty the delta it had just been given, so the boundary walks
the previously nonempty set first, skipping members that gained, then the
gainers. Any other member already has an empty delta at its end. The
boundary costs the two sets, not the component, so a cycle of *n*
predicates runs in *n* rounds of one member each, in linear time.

Setting a delta also raises each of the member's indexes to the delta's
start, where every lookup of the delta begins. Once an evaluation has begun
this is the only place a delta is written, so every delta lookup finds its
index prepared for it.

### A round's insertions may wait

No view of a round reaches past the end its relation had when the round began,
so what a round inserts is invisible to every plan of the round. A round may
therefore run its plans over the **frozen store**, the store as it stood when
the round began, gather what they derive, and insert it all after the last
plan, in derivation order: a **deferred round**. The same tuples take the same
identifiers, and the deltas, counters and rows are those of a round that
inserted as it went. This is what lets a round run on several threads, a
**shared round** ([parallel evaluation](design-parallel.md#model)); a shared
round that cannot finish runs again serially. Round 0, a non-recursive
component's pass, a query's group rules and a gathered query rule may be
deferred too, since none reads what it writes. A naive pass may not: each rule
reads what the rules before it in the pass inserted.

**Counters.** The context counts rounds, derivations (new or not, facts
seeded included), insertions, tuples scanned and tuples read from index
chains, for `relic_stats()` and `-X stats`; they are not a stable interface.
On a chain both fixed points take as many rounds as it is long, so tests
guarding against a regression to naive behaviour assert derivations and
scans, which is where semi-naive evaluation saves.

## When the relations are current

The derived relations are either the fixed point of the program as it
stands, or stale. They are current exactly when a recorded pair of counts --
rules and base-store tuples -- matches the program; anything else discards
every derived relation before evaluating from the facts. The record, the
discard and why relic discards rather than continues are owned by
[facts](design-facts.md#currency) ([the discard](design-facts.md#the-discard)).
What evaluation relies on:

* `relic_materialize()` (`-a`) returns at once when the relations are
  current; otherwise it discards, evaluates from the facts and records the
  pair.
* A query reads the fixed point when the relations hold it, and otherwise
  evaluates through its Magic rewrite, which discards the same way and
  leaves part of a fixed point with no pair recorded.
* A failed evaluation records no pair, so the next discards what it derived.
* The discard runs only before an evaluation begins or inside a load's
  commit, so no plan's pointer to a derived relation and no delta range spans
  it -- the obligation the store places on anything that ends a span of the
  append-only store ([storage](design-storage.md)).

## Answering a query

### The query object and its answer relation

A query compiles into a rule whose head is its output columns and names no
predicate ([planning](design-planning.md)). Its answer goes into an **answer
relation** the query object owns, of the query's output arity, created when
the query is prepared and released when it is destroyed.

**The answer stays out of the predicate table.** Predicate identifiers index
the relation table, the dependency graph, delta tracking, the discard walk
and the rewrite's arrays, several of which run once per answer. Were an
answer a predicate's relation, a session running one query per line would
walk every earlier line on each: quadratic. Making one walk incremental
would leave the rest, and the next per-predicate stage would join the list;
keeping answers out bounds every per-predicate cost by the program, by
construction. A query's group rules are likewise its own and never appended
to the program ([aggregates](design-aggregates.md)).

**Emptied at both ends of every run,** however the run ended. Emptying first
makes the contents right whatever the relation held; emptying after makes
the answer transient, counting nothing against the limits outside a run. The
context is busy throughout, so no other operation, a discard in particular,
sees an answer holding tuples.

**The totals are a sum.** Emptying a relation subtracts exactly what it held
from the context's totals, and a discard empties each derived relation that
way, so the totals are the sum of what every relation holds, in the table or
not, by arithmetic rather than by an ordering argument. The ordering
argument is asserted anyway in debug and sanitizer builds: a discard that
found derived relations to empty confirms afterwards that the totals equal
what the base stores hold, so an answer left non-empty fails a test instead
of skewing the account.

**Destroy reads nothing of the context.** An empty relation owns only its own
allocations, so a query may be destroyed after its context
([library interface](design-api.md)). The answer relation is not in the
relation table, which is indexed by predicate, so `relic_destroy()` releases
no answer relation: it never owned one.

**Pairing confirms the emit target fits.** Planning derives EMIT's schema
from the rule without a physical destination. Evaluation checks its arity
against the destination's input schema before execution. A key projection
also checks its column map and stored arity; scratch remains sized for the
original EMIT. A mismatch reports an internal error before a plan can read
or write beyond its row.

**`$q<serial>` is a display name.** The serial counts the queries a context
has compiled, the program's and prepared ones interleaved, a rejected query
taking none. `$q<serial>` names the answer in explain output, built when
printed and interned nowhere; like every generated `$` name it cannot collide
with a name a program writes ([naming](architecture.md#naming)). Diagnostics
call the answer "the query".

### Three steps

1. **Evaluate what the query reads.** When the relations are current, or
   under `-X no-magic`, that is materializing the program and the query rule
   is the one written. Otherwise [Magic Sets](design-magic.md) rewrite the
   query: the relations it reads completely are evaluated from the program's
   schedule, the generated rules from their own, and the query rule is the
   rewritten one. A rewrite that outgrows its limit is abandoned with a
   warning, and the query answered from the materialized program. A query's
   group rules run once each, after the program's part and before any
   generated rule, which may hold a copy of the aggregate; after an
   abandoned rewrite they run again, over the whole fixed point.
2. **Run the query's rule once.** No rule reads the answer, so one pass
   computes it completely. Duplicates collapse on insertion, which gives a
   query set semantics -- or, when the plan streams, its derivations are
   delivered as they are made.
3. **Deliver the answer**, one row per tuple in insertion order, each row's
   values borrowed for the call. A callback returning nonzero stops the rows
   and the run succeeds: the caller asked to stop, and nothing failed.

**No row before everything that can fail.** Every step that can fail -- a
limit, an allocation, an evaluation error -- happens in step 1, in planning
step 2, or in running it into the answer relation; delivering a gathered
answer cannot fail. A query that exceeds a limit produces an error and no
rows, never a truncated answer indistinguishable from a complete one
([limits](architecture.md#limits)). An abandoned rewrite falls back before
any row, since a rewrite is abandoned before its query rule runs. The one
failure that may follow the first row is the caller's own interrupt.

### Streaming

A query whose plan derives each row once delivers its derivations directly:
its `EMIT` hands each batch to the callback instead of inserting it, and the
answer relation stays empty throughout. Explain prints `stream` for such a
rule in place of `emit`. Inserting those derivations would reject nothing
and cost a hashed insert per row and a second copy of the answer.

**A distinct plan** is one in which no generator (`SCAN` or `LOOKUP`) has a
wildcard column -- an anonymous `_` or a dead variable -- and the `EMIT`
reads every slot a generator binds; slots an assignment or aggregate binds
are functions of earlier slots and need not be read. It is decided on the
built plan, after any rewrite, so it is true of what runs: `q(X), p(_, X)?`
streams when `q` is placed first and `p` becomes a test, while `p(_, X)?`
gathers.

**Why its derivations are a set.** The query rule runs once over relations
that do not change while it runs. A relation holds no tuple twice, a view
lists each tuple in its range once, and a test lets a binding through at
most once. Two tuples a generator reads under one binding of earlier slots
both match its constants and bound slots, so, holding no wildcard, they
differ in a column it binds; a repeated variable, as in `e(X, X)`, is bound
at its first occurrence and matched at the second, so this holds too. Each
path through the generators reaches the `EMIT` with a binding no other path
has, and an `EMIT` reading every generator-bound slot derives a tuple no
other path does. The relation would have rejected none and listed them in
derivation order, so the streamed rows are its rows, in its order.

A distinct plan streams only when, in addition:

* **it computes nothing and folds nothing.** An expression or a `sum` can
  fail on any binding, and a plan that can fail must not have delivered rows
  first; a computed value or a `count` has no identifier until the answer is
  stored. `min` and `max` gather with them, so whether an aggregate streams
  does not depend on which one it is;
* **no limit bounds the relations.** Under a limit a query delivers all its
  rows or none, because the refusal comes while the answer is gathered. A
  streamed row is never stored, so never refused: `e(A), e(B), e(C)?` over
  100 facts would deliver a million rows under `-l tuples=1000`. Whether a
  limit bounds the relations is answered beside the code that enforces the
  limits, by the test each check applies first, so the two cannot disagree,
  and `-X no-stream` never changes whether a query succeeds under `-l`;
* **`-X no-stream` (`RELIC_F_NO_STREAM`) is clear.**

**A streaming run allocates nothing.** Its plan and state are allocated
before it runs, every index it looks up was built when it was planned, its
`EMIT` inserts nothing, and no limit is set. So nothing between the first row
and the last can fail; the run ends early only by the callback's stop or an
interrupt. Debug builds assert that the store and arena totals and the
inserted-tuple count are the same after the run as before it. Every failure
the query's evaluation can meet happens in the components it reads, which
run first. A streamed row counts as a derivation, not an insertion, and a
note the rule reports comes between the rows delivered and the rest.

## Interruption

`relic_interrupt()` raises a flag the context holds and does nothing else.
The caller's contract -- the flag lowered on entry, the window between
operations where an interrupt is lost, which operations heed it, and
delivered rows forming a prefix of the answer -- is the
[library interface](design-api.md#interruption)'s. The evaluator's half is
where it looks for the flag and how long it can go without looking.

**The flag is read only where evaluation can already fail.** The serial
taker reads it after every return of the executor (`interrupted while
deriving path/2`), and a gathered answer before each row it delivers. An
interrupted evaluation leaves the context as any failed one does, and
nothing needs unwinding.

**The executor returns at least every quantum of work,** besides a full
batch and the plan's end. Work is counted two ways, each up to a fixed
bound:

* **backtracks.** Every turn either descends, at most as many times as the
  plan has operations, or ends in a backtrack, so backtracks bound turns;
* **rejected tuples,** which bound a scan rejecting tuple after tuple within
  one turn. A `SCAN` or `LOOKUP` that spends the count stops with its cursor
  in its state, and the executor records itself as about to ask it for its
  next match.

Either way the executor returns where a full batch would have left it and is
resumed the same way. A join that derives nothing for hours returns as often
as one that fills batches, and many plans that each finish at once are
stopped between plans.

**The overrun is bounded by one walk or one string operation.** A step with
no cursor to resume from cannot stop part way: an aggregate's fold, whose
running value is not kept between calls, and a keyed test that scans its
view, which happens only under `-X no-index` (a test naming no column reads
only the view's first tuple). Such a step charges what it reads to the same
count and, once the count is spent, finishes its walk and pauses the join as
it returns. So a call overruns its quantum by at most one such walk: a
group's chain or, without an index, the whole relation for a fold; the view
for a test. The quantum counts tuple reads, not work on values: a single
string function or regular expression match is not interrupted midway, so
the length of its operands bounds the delay too.

Index builds, merges, seeding, frontier moves and discards are bounded by
what the store holds and run whole; a frontier only moves forward, so its
walks over an evaluation together cover each chain once. Planning and the
Magic rewrite are bounded by the program and the rewrite's limit. An
interrupt made during any of them is noticed at the executor's next return.

The quantum does not change results: a pause leaves the state a full batch
leaves, so relations, answers, diagnostics and counters are the same
whatever the quantum.

**Delivered rows are a prefix, and the run returns -1.** A gathered answer
is interrupted while it is delivered and a streamed one while it derives;
why the interrupt is not held back until the answer is out is the
[library interface](design-api.md#interruption)'s.

**Workers abandon; the serial rerun reports.** A worker of a shared round
reads the flag after each return of its executor and, finding it raised,
abandons the job as it would for an error. The other workers stop at their
next claim, the candidates are released, and the calling thread runs the
round serially, meets the interrupt at its first look and reports it. A
worker never calls caller code or writes a diagnostic.

## Failure paths and limits

An evaluation fails, returning -1 with the failure reported where it
happens, when a configured limit refuses an insertion, an index build, an
arena growth or the interning of computed values; when an allocation fails;
when an expression's result or a `sum` is undefined, reported in its rule:

```text
rules.dl:9:35: error: invalid regular expression in rule hit/2: unmatched (
```

or when the caller interrupts it. Each stops the evaluation at once.
Planning a component can fail for memory or the memory limit, while an index
is built over a complete relation or the evaluation arena grows, and stops
the evaluation before that component runs. Nothing needs unwinding, which is
the payoff of the store's refuse-before-modify ordering: the relation that
refused a tuple is as it was, an index whose build failed was never
published, every other relation holds justified tuples, and the evaluation
arena is released. The next evaluation discards what the failed one derived.

Plans, operation states and the batch live in the **evaluation arena**,
reset when each evaluation ends and charged to the memory limit while it
lives ([memory](design-memory.md)). A fact has no plan, so a program of many
facts costs the arena nothing. Nothing plan-sized is allocated while the
executor runs; its data-sized scratch buffers are admitted and may grow
([the scratch](#the-scratch)).

## Determinism

Members are seeded in base-store identifier order, rules run in program
order and variants in planning order, relations iterate in insertion order,
and plans are a function of the program and the relations' sizes. Every
relation's tuples are therefore inserted in the same order on every run, and
rows arrive in insertion order, or in the derivation order equal to it when
streaming. A deferred round inserts in serial order, so the result is the
same at every thread count and whether or not a round is deferred. `-s`
exists for the caller who must not rely on any order
([determinism](architecture.md#determinism)).

## Alternatives not taken

* **Recursion in the executor.** It bounds rule length by the C stack and
  makes stopping mid-join for a batch or a quantum require unwinding.
* **Inserting from inside the executor.** The executor could then fail part
  way and write the context, each destination of a plan's derivations would
  need its own executor, and no worker could run it.
* **A scratch relation per delta.** It copies every delta tuple, builds and
  drops indexes each round, and points the delta literal at a relation whose
  identity changes each round. A range and a per-key frontier reach the same
  tuples in the same order with no copy.
* **Replanning a variant each round from its delta's size.** It costs the
  planner's work per round and makes explain output depend on how many
  rounds ran. Two plans built when the component is planned, chosen per
  round, give most of the benefit;
  one plan with a guessed delta size is badly wrong on some round for some
  bodies ([planning](design-planning.md)).
* **A separate reference evaluator in the test suite.** It would test a
  duplicate rather than the product ([testing](design-testing.md#the-oracle)).
* **An answer predicate per query, one shared per arity, or one answer
  relation owned by the context.** The first grows the predicate table with
  the number of queries; the second makes two live queries one predicate,
  correct only because answers are transient and the context is not
  re-entrant; the third gives a relation an arity and name that change
  between answers, and two prepared queries shared state.
* **Keeping an answer across runs.** A cached answer a later load silently
  invalidates is worse than one recomputed.
