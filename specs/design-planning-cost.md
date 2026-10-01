# relic planning cost design

**Status.** Design document for what bounds the work of planning: why
ordering a body costs what its plan holds rather than the square of its
length, and why the memory an evaluation's plans and a query's rewrite occupy
is charged to the same limit the data is. Forward-looking, like every
document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/design-planner.md` section 5.1 for the order this
document computes more cheaply and never changes; `specs/design-eval.md` for
the delta variants that multiply a rule's plans; `specs/design-magic.md`
sections 8 and 11 for the rewrite's copies, its magic rules and its arena;
`specs/design-relations.md` section 9 for the memory limit;
`specs/design-memory.md` sections 2 and 4 for the arena lifetimes and the
failure flag; `specs/decisions.md` D-6, D-7, D-10 and D-18.

---

## 1. What this document settles

* the four factors that multiply into the cost of planning a rule, and
  which of them are inherent;
* the invariant that makes the greedy order cheap: a bound set that only
  grows and has at most 65 states;
* the ranking that computes the greedy order in that many passes, and why it
  chooses exactly what the step-by-step comparison chose;
* the bound this puts on planning time, in terms of the plans' size;
* the memory account: the arenas holding an operation's working state are
  charged to `max_memory`, so that a limit on memory is a limit on planning
  time;
* how a refusal by the account is reported, through the one function every
  allocation failure already reports through;
* the workloads that hold the bound.

It does not change what a plan is, which plan is chosen, or the text of `-e`.
Two runs of the same input produce the same plans before and after this
document, which is what makes it a cost question and not a semantics one.

---

## 2. Where the cost of planning comes from

A rule of *N* body literals costs, before its first tuple is derived, the
product of these factors.

1. **Ordering one plan.** Each step of the greedy order of
   `specs/design-planner.md` section 5.1 scores every positive literal not
   yet placed, and after each placement offers every unplaced test a place.
   Done as a rescan of the body per step, that is *N* scores and *N*
   readiness checks per step, *N* steps: the square of the body.

2. **Delta variants.** A rule over a recursive component gets one variant
   per literal that reads a member of the component, per
   `specs/design-eval.md` section 4.1, and under `-X naive` a plan instead.
   A variant orders its body once, or twice when the placed order puts
   another literal before the delta literal, and holds one or two plans of
   *N* + 1 operations, per `specs/design-evaluation-cost.md` section 4.3. A
   body whose literals all read the component gets *N* variants: up to 2*N*
   times the ordering cost, and up to twice the square of the body in
   memory.

3. **Magic copies.** The rewrite of `specs/design-magic.md` copies every rule
   deriving a predicate once per adornment of it, and orders each copy's body
   through the same greedy order. *R* call patterns of one rule cost *R*
   times the ordering cost and *R* copies of the body.

4. **Magic prefixes.** For each call in a rewritten body that reads a
   predicate on demand, the rewrite generates a magic rule whose body is the
   prefix before the call. Written out in full for every call, the plain
   construction, a body of *L* such calls generates *L* rules holding about
   *L* squared over 2 literals between them, and when those rules are
   recursive in the rewritten program each gets its own delta variants,
   about *L* cubed over 3 operations in all. A rule of four hundred calls on
   one demanded predicate, under five kilobytes of text, takes seconds and
   over a gigabyte to answer through a rewrite sized to admit it, and with
   no Magic limit set the budget on literals abandons it. The
   supplementary construction of `specs/design-magic.md` section 6 derives
   each prefix from the one before it, so the generated bodies are linear in
   *L*.

The second and third factors are inherent: semi-naive evaluation needs a
variant per occurrence, and an adornment needs a copy per pattern, and each
plan or copy is output the evaluation goes on to use. The first is not. It
comes from recomputing, at every step, answers that the step before already
had. The fourth is removed by the supplementary construction, and what remains
of the rewrite's size is the third: a rule copied whole once per adornment.
The rewrite holds D-7's invariant, that it generates no more than a constant
multiple of the program, by budgeting the literals it generates as it
budgets the rules, per `specs/design-magic.md` section 8: a rewrite whose
adornments multiply the program is abandoned with the usual warning and the
query answered from the whole program. This document bounds what the budget
admits the way it bounds the other factors, by charging what it holds.

Multiplied out, a rule of a thousand identical recursive literals -- six
kilobytes of text -- takes seconds to plan and tens of megabytes to hold, and
the same rule called with hundreds of bound patterns takes tens of seconds
through the rewrite. All of it is paid before any tuple is derived, and none
of it is counted by any limit.

---

## 3. The invariant: a bound set that only grows

The planner's state during ordering is the set of slots bound so far, a
64-bit mask over the rule's variables of D-7. Two facts about it are what this
document rests on.

* **It only grows.** Placing a positive literal adds the slots the literal
  binds and removes nothing. Placing a test adds nothing, except an
  aggregate literal, a test that binds, which adds its result slot and
  removes nothing either, `specs/design-aggregates.md` section 8.1.
* **It has at most 65 states**, the one it starts from and one per bit that
  can still be set, because it is monotone over 64 bits. A rule of ten
  thousand literals still passes through at most 65 bound sets.

Everything the greedy order asks depends on that mask and on inputs that do
not change while one plan is ordered:

* a positive literal's score -- its bound-argument count and its estimate --
  is a function of the literal, the mask, and statistics that are fixed for
  the plan: the counts and distinct counts of complete relations, and the
  set of relations still being derived (`specs/design-planner.md` sections
  3.1, 4 and 5.1). Nothing inserts a tuple while a plan is ordered, and a
  relation looked up for its statistics is created empty if it did not
  exist, which changes no score;
* a test is ready exactly when the slots it reads are within the mask.

So between two placements that leave the mask unchanged, no score changes
and no test becomes ready. In the thousand-literal rule above the delta
literal binds the one variable, and the remaining 999 placements never change
the mask; a rescan per step recomputes the same 999 scores 999 times.

---

## 4. The ranking

The greedy order is computed in passes, one per state of the mask, rather
than in steps. "Pass" here is a pass of the planner over the body, and not
a round of the fixed point.

**A ranking** is the positive literals not yet placed, sorted by the key of
`specs/design-planner.md` section 5.1: more bound arguments first, then the
smaller estimate, then the earlier body position. It is computed against one
mask and is valid while the mask is that.

The order is decided as follows, from the mask the caller starts the planner
with: nothing bound for a rule's plan, the head's bound columns when the
Magic rewrite orders a body for a call.

1. Place the tests that are ready against the starting mask. When the
   order is a variant's delta-first order, place the delta literal, and then
   the tests it made ready. In every other order a delta literal is ranked
   with the other positive literals, by the estimate of its view,
   `specs/design-planner.md` section 5.1.
2. If no positive literal is left, the order is complete. Otherwise score
   every unplaced positive literal against the mask and sort them into the
   ranking.
3. Place the first literal of the ranking and drop it from the ranking.
4. If the mask is unchanged and the ranking is not empty, go to step 3.
5. If the mask grew, place the tests that became ready, in body order.
   Go to step 2.

Under `-X no-reorder` step 2 sorts by body position alone and scores
nothing, so the order is the one the reference of `specs/design-planner.md`
section 6 defines, the delta literal in its written place, within the cost
bound below rather than the quadratic one.

**Why it chooses what the step-by-step comparison chose.** At every step the
greedy order places the unplaced positive literal that is least by the key,
comparing against the current mask. The first of a ranking computed against
that mask is that literal. After placing it without changing the mask, every
remaining literal's key is exactly what it was, so the least of the rest is
the next of the ranking. When the mask changes the ranking is recomputed,
which is the step-by-step comparison starting over with the new mask. The key
ends in body position, which is distinct per literal, so no two literals
compare equal and the sort has no ties to break by accident: the
deterministic order of `specs/design-planner.md` section 9 is the same order.
A score that fails -- a statistic that could not be computed -- fails the
same literal at the same point in both, because a pass scores in body order.

**Tests** are offered a place only when the mask has grown, because that is
the only event that can make one ready. Between growths a scan would find
nothing.

**What it costs.** Each pass scores at most *N* literals, sorts them in *N*
log *N* comparisons, and scans *N* tests. There are no more passes than
positive literals, and no more than 65, because each pass after the first
follows a growth of the mask. One plan therefore costs at most 65 x *N* log
*N* rather than *N* squared. A short body of *k* positive literals that binds
something at every placement still takes *k* passes, and costs what a rescan
per step would within a logarithm; the bound matters for the body that is
long, where the mask must stop growing long before the body ends. The
planner's record of what is placed is scratch of the plan's own length,
allocated beside the plan in the same arena, so it is charged with it and is
of its order. The ranking is scratch of the same length, a few words per
literal against the byte of that record, and is freed when the plan is built:
kept beside every plan an evaluation holds, it would add a quarter to the
memory of the long bodies this document is about. Held for one call only, it
is of the order of the rule the caller wrote.

---

## 5. What this bounds

A plan of *N* literals holds *N* + 1 operations, and section 4 orders it in
at most 65 x *N* log *N* work. So **the time to plan is bounded by the size
of the plans produced**, within a factor logarithmic in the longest body.

A delta variant may decide an order it does not build. When the placed
order puts another literal before the delta literal, the variant also
decides the delta-first order, and each of the two plans is then built only
if some delta size would select it. A variant decides at most two orders of
the same body and builds at least one plan of *N* + 1 operations, so its
planning time is at most twice what ordering one of its plans costs, and the
bound holds within that factor of two.

The delta variants of section 2 multiply the plans, the Magic copies and
prefixes multiply the rule bodies, and each multiplies the time by what it
adds to the rewritten program and its plans.

That reduces bounding planning time to bounding the memory plans and
rewritten rules occupy, which the next section does. The residual factors of
section 2 are then bounded the way the data is: a program can ask for plans
in proportion to the square of its longest body, and the limit on memory
says how many it gets.

---

## 6. The memory account

`max_memory` counts the storage of relations and indexes, per
`specs/design-relations.md` section 9, and no arena, on the reasoning that an
arena is sized by the program and the limit exists to bound what the data
can do. Section 2 shows the reasoning does not hold for the two arenas that
hold an operation's working state: the evaluation arena holds plans whose
count and length are each the length of a body, and the rewrite arena holds
a copy of every body per pattern the program calls it with, and a prefix per
call. Each is the square of what the program author wrote, at the author's
choice, and the plans of the rewritten program its cube; the evaluation arena
is bounded by nothing, and the rewrite arena only by budgets that count rules.

**The account** is the context's total of the bytes an operation holds on
the user's behalf: the relation and index storage that
`specs/design-relations.md` section 9 already totals, plus the bytes the
arenas of operation lifetime hold from the allocator. The limit is enforced
against that total wherever it grows: the relation store checks it before an
insertion, an index build or a load's commit to the base stores, and a
charged arena checks it before it obtains a chunk.

**Which arenas are charged** is decided by what they hold, in the terms of
the lifetimes of `specs/design-memory.md` section 2. The evaluation arena
and the rewrite arena hold what an operation multiplies out of the program:
plans, whose number and length are each a body's length, and rewritten
rules, a copy per pattern and a prefix per call. They are charged. The
context, program, schedule and query arenas hold the program and the query
as the caller wrote them, and are not. The analysis scratch and the schedule
that a query's rewritten program gets, released with the rewrite, are linear
in the rewrite and are not charged either: they are a constant fraction of
what the rewrite arena already pays for. Two things keep the caller's
arenas out.

* The limit bounds what the program's stores hold and what the program
  *does*, and the program as written is bounded by the caller, who chose the
  file; `TODO.md` records why input size is a separate key and not a wider
  meaning for this one. The facts a program states are held in base stores,
  which the relation store counts (`specs/design-facts.md` section 8), so the
  tuple and memory limits bound them, and a load whose facts do not fit is
  refused while loading, with nothing added. Charging the program arena as
  well would count the text of the program's rules against a limit meant
  for what the program holds and derives.
* A query's arena may outlive the context, per `include/relic.h`: a query is
  destroyed before or after its context. An arena charged to an account the
  context owns cannot be released after the context is, so a caller-owned
  arena cannot be in the account at all.

A fact is not a rule of the program and is not planned, and neither is a
body-less seed of a rewrite (`specs/design-eval.md` section 3), so the
evaluation arena holds the plans of rules with bodies. Were each fact
planned, its plan would be several times the size of its tuple, and charging
the arena would refuse a program of many facts under a limit that admits
their base store.

An arena is attached to the account when it is initialized, and a reset
returns its bytes to the account and keeps the attachment, so a phase that
ends gives back what it held and the account is the sum of what the store and
every charged arena hold. The two initializations, attached and not, are
distinct functions rather than one with an optional argument, so that an
arena's lifetime class is a choice its creator makes in the open and a
reviewer can see.

**What the knob means** widens accordingly, in `include/relic.h`,
`man/relic.1` and `man/relic.3`, once implemented: `max_memory` bounds the
bytes of relation and index storage, the base stores of the program's facts
included, plus the working state of an evaluation and of a query's rewrite.
It does not count the program's rules and queries, a load's text and its
staging, the interned symbols and values, the schedule, or the growable
tables of the program and the rewrite, each of which is linear in something
the caller wrote or in something the account or a budget already bounds.
Zero still means unlimited, so under the default configuration planning
memory is bounded by the allocator alone, exactly as the data is: the limits
of D-7 are the user's, and a program that plans itself into gigabytes under
no limit is the same event as one that derives itself there.

**What it checks.** A charged arena checks the account before it obtains a
chunk, against the chunk's whole size, so that a limit which fires has
prevented the allocation rather than reported it, as the relation store's
check does. The chunk sizing of `specs/design-memory.md` section 3 is
unchanged: an arena that has room in its head chunk asks the account nothing,
so the check is paid once per chunk and not per allocation. The limit can
therefore fire up to one chunk before the total would have crossed it.

---

## 7. How a refusal is reported

An arena that the account refuses fails the same way an exhausted allocator
fails it: the request returns `NULL`, the sticky flag of
`specs/design-memory.md` section 4 is set, and every later request is refused
until the reset. Nothing about the contract callers hold changes.

What is reported differs. An exhaustion is reported as out of memory; a
limit is reported in the form every limit shares, of
`specs/design-relations.md` section 9: which limit, what it was set to, and
what was being done. Every place that turns a failed arena into a diagnostic
already does so through one function, the one that reports exhaustion, and
that function is where the difference lives. **When the account refuses, it
records on the context that the memory limit did**, together with the
operation in progress, which the context records once when an operation
begins: evaluating a component, named by its first member; planning a
query's rule, named as the query is in diagnostics; rewriting a query. The
reporting function, finding that record, formats

```text
limit exceeded: memory=N while planning <predicate>
limit exceeded: memory=N while planning <query>
limit exceeded: memory=N while rewriting <query>
```

instead of out of memory, and consumes the record. No site that reports an
allocation failure changes, whichever arena it confirms and whether it
reports at a boundary or at the allocation, because the account and the
reporting function are the only two parties to the record. An arena reset,
or the clearing of a flag, does not touch the record: it belongs to the
refusal, not to the arena. An operation clears the record and the recorded
operation when it begins, so that a refusal in the allocations an evaluation
makes before its first component is reported as the evaluation's, and a
record no diagnostic consumed cannot be reported by a later operation as its
own.

The refusal is a failure of the operation, as a limit refusing a tuple is:
the evaluation or the query fails, nothing is answered, and the next
evaluation discards and starts again, per `specs/design-eval.md` section 8.
The rewrite's budgets of `specs/design-magic.md` section 8 are unchanged. A
rewrite can reach the memory limit before it outgrows a budget, and then the
query fails rather than being answered from the whole program: the budget is
the rewrite's own measure of whether it is worth having, and the limit is the
user's ceiling on the process, and a rewrite that reaches the ceiling is one
the user asked not to pay for.

---

## 8. The workloads

Two workloads hold the bound, in `tests/perf.sh` beside the ones already
there and measured on the clock as the recursive cycle there is: linear in
the plans, each takes about a second; square in the body, minutes.

* **A long recursive body.** `p(a). q(a). p(X) :- p(X), q(X), ..., p(X),
  ... . p(X)?` with some eight thousand literals, one in sixteen reading
  `p`: five hundred delta variants of eight thousand operations, ordered in
  a pass or two each because the mask grows once. Mixing in a complete
  relation keeps the plans' size at a few million operations while making
  each plan long, which is what separates the linear order from the
  quadratic one by more than any difference between machines.
* **A long body under many patterns.** A predicate of an
  eight-thousand-literal body, reading complete relations only, called with
  a hundred or more distinct bound patterns and queried through a predicate
  that reads them all, so that the rewrite orders each copy once and
  generates one magic rule per pattern.

Both also run with `max_memory` set below what their plans need, and must
fail with the limit message of section 7 before the plans are built, in
about the time the limit allows for.

The golden plan tests are the proof that section 4 chose the same order:
every `-e` line is unchanged. What does change is where a very small limit
fires: the evaluation arena's first chunk is charged before any tuple is
derived, so a limit of a few kilobytes that today refuses the first tuple or
the first index refuses the evaluation's plans instead, and the tests that
set such a limit expect the planning message of section 7.

---

## 9. Determinism

* The ranking's key ends in body position, so the sort has one result.
* A pass is recomputed at the same masks on every run, because the masks
  are a function of the order and the order is a function of the rankings.
* The account is a sum, checked at the same chunk allocations on every run,
  so on one platform a limit that fires fires at the same point; chunk
  headers differ between platforms, and so may the point.

---

## 10. What is deliberately absent

* **A limit on body length.** With section 4 in place a long body costs what
  its plans hold, and section 6 bounds that when the user bounds memory; a
  structural limit like the arity and variable limits of D-7 would buy no
  representation and would be a restriction that a generated program can
  meet.
* **A default budget on plans.** The Magic budgets of D-7 exist because the
  rewrite is optional and has a fallback, and the literal budget of section
  2 is that mechanism applied to the rewrite's size: it bounds the literals
  the rewrite drafts, not the plans and delta variants that evaluating them
  builds. It is the supplementary construction, which keeps every prefix
  after a body's first demand call to one literal wherever its live
  variables fit a predicate, that keeps those plans linear in the body. A rule's plans and its delta variants are the
  evaluation itself: the naive evaluator of D-9 is the
  reference the semi-naive one is tested against and not a fallback the
  library would silently choose, so a budget that refused a plan would refuse
  the program. Planning memory is unlimited by default because the data is.
* **Fewer delta variants.** Semi-naive evaluation needs one per occurrence
  of a member, and a variant is output rather than working state.
* **Deduplicating repeated literals.** It would settle the example of
  section 2 and no other body.
* **Charging the distinct-count scratch table.** It is a few bytes per tuple
  of one relation, released as soon as the count is known, and the relation
  it is sized by is one the memory limit already admitted
  (`specs/design-planner.md` section 3.1).
* **A memoized ranking across variants.** The delta variants of one rule
  begin at different masks and each orders in a few passes; sharing passes
  between them would save little and would tie the variants' plans together.
* **Rescoring only the literals a growth touched.** A pass could rescore
  only the literals that mention a newly bound variable and keep the rest
  ranked, for a bound without the factor of 65. The factor is a constant and
  the passes are few, and the simpler pass is the one a reader can check
  against section 5.1.
