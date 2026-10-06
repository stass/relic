# relic existence check design

**Status.** Design document for the existence check: the operation a
positive literal becomes when it binds nothing the rest of the rule reads,
so that it passes each binding on at most once instead of once per tuple
it matches. Forward-looking, like every document under `specs/`; `TODO.md`
records what is not yet implemented.

**Companions.** `specs/design-plan.md` sections 3, 4 and 5 for the
operations, the terms and the binding analysis this document extends;
`specs/design-planner.md` sections 5, 6 and 8 for the order, which this
document never changes, the references and the text of `-e`;
`specs/design-program.md` section 6 for the singleton warning, which
reports the variable this document is about; `specs/design-negation.md`
section 4 for the probe this operation shares;
`specs/design-evaluation-cost.md` sections 3 and 4 for the views and the
threshold between a variant's plans; `specs/design-comparison-cost.md`
section 4.1 for the plans that stream; `specs/design-planning-cost.md`
sections 3 and 4 for the invariant and the passes that keep planning
cheap; `specs/design-index.md` section 4 for where a lookup of a delta
starts; `specs/design-magic.md` section 5 for the bound set the rewrite
follows through a body; `specs/decisions.md` D-4, D-8, D-9 and D-10.

---

## 1. What this document settles

* why a positive literal that binds nothing anything reads multiplies the
  rows of a join, and by how much;
* which variable is **dead**: one the rule mentions once, of which `_` is
  the nameless case;
* the operation, `EXISTS`: a test keyed on the literal's bound columns,
  answered by the probe a negated literal already makes, through a view;
* what the planner changes, which is the choice of operation alone, and
  what it leaves alone, which is the order and the estimates;
* the executor's one new case, and what each place that switches over the
  kinds of operation decides for it;
* `-e`, the reference `-X no-exists`, and the tests that hold the
  equivalence;
* what this does not do.

It does not change which tuples a rule derives or in what order the
planner places a body. Two runs of the same input derive the same
relations with and without the operation, which `tests/equiv/strategies.sh`
and `tests/random` check.

---

## 2. A binding nobody reads multiplies the join

A generator succeeds once per tuple it matches and leaves that tuple's
unbound columns in the frame, `specs/design-plan.md` section 3. That is
right when something later reads them: each tuple is a different
continuation. It is waste when nothing does, because every tuple then
continues the join with the same frame, and the operations after it run
once per tuple for the same result.

```prolog
p(a, x).
p(b, x).
s(X) :- p(_, X), p(_, X), p(_, X).
```

The first literal scans `p` and binds `X`, twice, to `x`. The second has
`X` bound and `_` in its first column, so it is a `LOOKUP` keyed on column 1
with an `ANY` term in column 0: it matches both tuples and continues twice
with the frame it was given. So does the third. The `EMIT` runs eight times
to derive `s(x)` once; with *k* such literals it runs 2^*k* times. A rule of
48 calls `p(_, ..., X, ...)` over two facts of `p` takes 4.7 seconds to
answer `s(X)?` from the whole program, and one of 63 runs for minutes, for
one row that the first literal alone determined.

The cost is not the relation's and not the limits' business: every
relation here is two tuples, and `max_tuples` and `max_memory` bound what
is held, not what is read. `relic.3` says that no limit bounds the time an
evaluation takes, and that stays true. What this document removes is a
cost a program pays for bindings it never uses, not the bound the manual
declines to promise: a program whose head reads every column of every
literal costs the same with and without it.

The literal's own answer is a yes or a no: does the relation hold, in the
view the literal reads, a tuple that agrees with its bound columns? That
is the question a negated literal asks, with the answer inverted,
`specs/design-negation.md` section 4, and the executor already answers it
in constant time through the canonical index or a secondary one. A
positive literal whose bindings nothing reads is that question with the
answer kept.

---

## 3. A dead variable is one the rule mentions once

**A variable is dead when the rule mentions it exactly once**, head
included, and that mention is in a positive literal. `_` is the nameless
dead variable: it holds no slot because nothing can read it,
`specs/design-plan.md` section 7, and it becomes an `ANY` term that
matches anything and binds nothing. A named dead variable is exactly that
with a name, so `p(X) :- e(X, _Y)` and `p(X) :- e(X, _)` are one rule to
the plan. The singleton warning of `specs/design-program.md` section 6
reports a dead variable whose spelling does not begin with `_` and
advises renaming it to one that does; `_Y` is the author's statement that
the column is ignored, and this document makes the plan take the
statement at its word, warned or not.

**Why one mention is the whole condition.** Take a variable held by a
positive literal *L* and unbound when *L* is placed in a decided order.
No literal before *L* in the order holds it: a positive literal there
would have bound it; a comparison, a negated literal or an aggregate's key
is placed only once every slot it reads is bound; an assignment or an
aggregate that mentions it as its result binds it. So "unbound at *L* and
held by nothing after *L*" says "held by *L* and by nothing else in the
rule", and "held by *L* once", since a second column of *L* that holds it
reads what the first bound, as `edge(X, X)` finds loops. That is one
mention in the rule. The converse is immediate: a variable mentioned once,
in a positive literal, is bound by nothing else and read by nothing. The
two conditions are the same set, and the second is a property of the rule
alone, not of the order, which is why the planner can read it off the rule
before any ordering starts, as a 64-bit **dead mask** over the rule's
slots.

**What a literal mentions** is defined by one helper the counting pass
uses, so that the pass and the argument above cannot disagree:

* a positive literal mentions every variable in each of its columns, a
  variable in two columns twice;
* a negated literal mentions every named argument, `_` being none;
* a comparison mentions every slot of both expressions;
* an aggregate literal mentions its result slot and its shared slots, the
  variables its body shares with the rule; the body's own variables belong
  to the group rule compiled for it, `specs/design-aggregates.md` section
  5, and are not the rule's;
* the head mentions every variable in each of its columns.

The pass walks the rule once when the planner starts and drops from the
mask every slot it sees twice. The compiled rule's record of a variable's
occurrences, which the singleton warning reads, is not what the planner
reads, because a rule the Magic rewrite generates carries a synthetic
count there. A query's synthetic head holds every named variable,
`specs/design-plan.md` section 7, so a query's only dead variables are its
`_`s and a `_Y` in a query is an output column, live. A group rule holds
every variable of its body in its head and has none either. An aggregate's
result is never in a positive literal and is never dead.

**A dead variable has no binding.** The planner's bound set grows, in the
order and in the plan alike, by the slots of a literal's variables that
are not dead. The two walks therefore agree on every bound set, and the
invariant of `specs/design-planning-cost.md` section 3 -- a set that only
grows, through at most 65 states -- holds as it did. Section 5 says why
nothing the order depends on can tell the difference.

---

## 4. The operation

```c
enum relic_op_kind {
	RELIC_OP_SCAN,		/* every tuple of a relation, matching and binding */
	RELIC_OP_LOOKUP,	/* the tuples an index finds for a bound key */
	RELIC_OP_MEMBER,	/* is this fully bound tuple present? */
	RELIC_OP_EXISTS,	/* does some tuple agree on the bound columns? */
	RELIC_OP_ANTI_MEMBER,	/* does no tuple agree on the named columns? */
	RELIC_OP_FILTER,	/* does a comparison of two bound values hold? */
	RELIC_OP_ASSIGN,	/* bind a slot to a value computed from bound ones */
	RELIC_OP_AGGREGATE,	/* fold a group relation's rows for a bound key */
	RELIC_OP_EMIT		/* derive the head tuple */
};
```

`EXISTS` is a test: it reads only constants and bound slots, binds
nothing, and succeeds at most once. It extends the table of
`specs/design-plan.md` section 3 by one row:

| Operation | Reads | Binds | Succeeds |
| --- | --- | --- | --- |
| `EXISTS` | every column that is not `ANY` | nothing | once or never |

**The rule, stated once, per column.** When a positive literal is placed,
each of its columns is one of four things, decided in this order:

* a **key column**: it holds a constant, or a variable whose slot is bound
  before the literal; a `CONSTANT` or `BOUND` term, and a bit of the key;
* an **`ANY` column**: it holds `_` or a dead variable; an `ANY` term;
* a **binding column**: it holds a variable whose slot is unbound and that
  is not dead, and is the first column of the literal to hold it; a
  `BIND` term, which binds the slot;
* a **matched column**: it holds a variable a binding column of the same
  literal binds; a `BOUND` term outside the key, matched against the
  value the binding column just bound, as `edge(X, X)` finds loops.

The precedence settles the two cases that could be read two ways. A
variable bound before the literal is a key column whatever its mention
count, so `Y = 3, p(X, Y)` keeps its equality; and a dead variable is
never a bound slot, since nothing else holds it, so the first two cases
never meet. The key is exactly the columns bound before the literal, which
is what a `LOOKUP` keys on and what `specs/design-plan.md` section 4
promises: a `BIND` and a matched `BOUND` never lie in a key column. The
literal becomes:

* `MEMBER`, when every column is a key column, which is the rule
  `specs/design-plan.md` section 5 gives for a literal whose every
  argument is a constant or a bound slot;
* `EXISTS`, when it has no binding column and some `ANY` column; it has
  no matched column either, since a matched column needs a binding one;
* a generator, `SCAN` or `LOOKUP`, when it has a binding column; its
  `ANY` columns, dead variables among them, match anything and bind
  nothing, as a `_` does.

`MEMBER` and `EXISTS` are two shapes of one fact, a literal that binds
nothing, and `MEMBER` keeps its name because it is the case a reader of
`-e` output and of the documents knows. After this rule **every generator
binds a slot something reads**: a literal whose unbound variables are all
dead is a test. The multiplication of section 2 came from generators whose
bindings reached nothing, and no plan holds one.

**Terms.** An `EXISTS` carries `relic_op_atom` as an `ANTI_MEMBER` does: a
`CONSTANT` or `BOUND` term for each key column, an `ANY` term for each
`ANY` column, a key mask with a bit per key column, the index the key
names when it names one, and the view the literal reads. The invariant of
`specs/design-plan.md` section 4 widens by one name: `BIND` occurs only in
a `SCAN` or a `LOOKUP`; `ANY` occurs in those, in an `ANTI_MEMBER` and in
an `EXISTS`; neither ever occurs in a key column. A dead variable has no
term that names it, as a wildcard has none, because the plan reads nothing
there.

**The key's shape decides how the question is asked**, exactly as a negated
literal's does, `specs/design-negation.md` section 4, and through the same
executor code:

| Key | Shape | How it is answered |
| --- | --- | --- |
| every column | full | a `MEMBER`: the canonical index finds the tuple, and it is inside the view |
| some columns | partial | the secondary index keyed on exactly those columns: the key's first tuple inside the view exists |
| no column | empty | the view is not empty |

The partial shape is the one section 2 is about. Its index is the one a
`LOOKUP` on the same columns would read, obtained when the literal is
placed by the same call and built then if the relation has none, charged
to the memory limit as every index is; `specs/design-planner.md` section
5.2 and `specs/design-index.md` section 7.

**A view.** A negated literal reads through `all`, since the relation it
reads is complete, and its probe reads the first tuple of the key's chain.
A positive literal reads through whichever view its place in a delta
variant gives it, `specs/design-plan.md` section 5.3, and so does its
check: the delta literal of a variant may be an `EXISTS` of the delta, and
a member literal beside it an `EXISTS` of `old` or `current`. The probe
therefore begins where a `LOOKUP`'s walk through the same view begins --
at the chain's head for `all`, `old` and `current`, and for `delta` at the
key's first tuple at or past the delta's start, which the index finds
through the key's frontier, `specs/design-index.md` section 4.1 -- and
finds a tuple exactly when that walk would find its first: the chain lists
its tuples in increasing identifier order, so its first tuple from the
start is its lowest, and some tuple of the key lies inside the view
exactly when that one is below the view's end. A member's every index
learns the delta's start when the delta is set, so a check of a delta asks
its index the question a lookup of the delta asks, walks the frontier as
the lookup does, and may run beside lookups and checks of the same key on
other threads by the argument `specs/design-index.md` section 4.1 makes
for lookups at one start. An `ANTI_MEMBER`'s probe is the `all` case of
this.

**Without indexes.** Under `RELIC_F_NO_INDEX` the partial shape scans the
view for the first tuple the terms match, with the matcher a `SCAN` uses,
as a negated literal's partial probe does. The scan has no cursor to
resume from, charges every tuple it reads to the call's quantum and
finishes before the executor may pause, `specs/design-interrupt.md`; a
call overruns its quantum by at most one such scan, and the scan does no
more work than the generator it replaces, which read at least as far to
find the same first match. The full and empty shapes read no secondary
index and are unchanged by the flag.

**Counters.** A check counts what the probe it shares counts: the partial
shape one tuple looked up when it finds one, the empty shape and the
partial shape under `-X no-index` each tuple scanned, and the full shape
what a `MEMBER` counts. The counts are a function of the bindings met and
stay identical across thread counts, which `tests/langtest.sh` requires.
The `derivations` counter falls for a rule with a check, by construction:
that is the work the operation removes.

---

## 5. What the planner does

**The order is unchanged.** Three facts about the ranking of
`specs/design-planning-cost.md` section 4 make that so, with dead
variables left out of the bound set.

* A literal's score reads the bound set only through its own columns, and
  a dead variable is in no other literal's column, so no score changes
  when a dead variable is or is not bound.
* A placement that grew the bound set by dead slots alone ends a pass of
  the ranking; without those slots it does not, and the next literal is
  taken from the ranking already computed. A pass recomputed there would
  sort the same scores by the same key, which ends in body position and so
  has one result, into the same ranking.
* A test is offered a place when the bound set grows, and a test reads no
  dead slot, so no test is placed earlier or later.

So a program's `-e` output lists the same literals in the same order,
with `exists` in place of `lookup` or `scan` for a literal that binds
nothing anything reads, and `_` in place of a named dead variable in a
generator's column.

**The choice is made where the literal is placed**, by the rule of section
4 against the bound set and the dead mask, as `MEMBER` against `LOOKUP` is
made against the bound set alone. Whether a literal is a check is
therefore a function of the same state the ranking reads, and the ranking
could weigh it; section 11 says why it does not.

**A variant may hold a literal as a check in one plan and a generator in
the other.** A delta variant decides up to two orders of the same body,
`specs/design-planner.md` section 5.1, and whether a literal's columns are
bound depends on what is placed before it. A literal placed after every
other literal holding its variables has only key and `ANY` columns there
and is a check; the same literal placed before them binds and is a
generator. Both plans read every literal through the same view, so they
derive the same tuples and differ only in cost, which is the property a
variant's two plans have.

**The estimates and the threshold are unchanged.** A positive literal is
scored by the tuples it would match for a binding, `specs/design-planner.md`
section 4, and a variant's prefix and fan-out estimates multiply the
estimates of every positive literal but the delta literal, `MEMBER`
literals among them, `specs/design-evaluation-cost.md` section 4.3. An
`EXISTS` is scored and multiplied as a `MEMBER` is: by the tuples that
agree with its key, though it passes each binding on at most once. The
imprecision is a test's, not this operation's, and section 11 leaves it
to the cost model, so that every order and every threshold the golden
plans record stays as it is.

**The Magic rewrite is untouched.** It asks the planner for an order alone,
`specs/design-planner.md` section 7, and follows the order it gets by what
each literal binds, `specs/design-magic.md` section 5, to find the columns
each call has bound and the variables live after each step. A call can
hold a dead variable, but no dead variable is bound at any call, since
nothing binds it before the literal that holds it and that literal is the
call; so the columns a call binds, and the adornment they make, are what
they were, and the live set, the bound slots some later literal reads,
holds no dead variable with or without this document. The rules the
rewrite generates are planned by the same walk and get existence checks
where their bodies have dead variables, with no case of their own.

---

## 6. What runs it

**The executor** gains one case in the operation dispatch, beside
`ANTI_MEMBER`'s, with the inversion left out:

```c
case RELIC_OP_EXISTS:
	if (resume)
		return (false);
	return (step_probe(scratch, &op->u.atom, &state->view));
```

A test asked to resume rejects, as every test does, which is what makes it
succeed at most once; the loop of `specs/design-eval.md` that runs a plan
does not change. The partial shape's probe takes the view it is given
rather than assuming `all`, and begins at the delta's start through the
frontier when the view is `delta`, per section 4; the full and empty
shapes read a view by its range as they do.

**Every switch over the kinds decides the new one.** Each place that
switches over `relic_op_kind` does so without a default, as
`specs/design-comparison-cost.md` section 4.1 asks, so that the build fails
until each has decided what an `EXISTS` means to it. The decisions:

* the executor resolves its view as it does a `MEMBER`'s, and the round
  names its relation as one read as the plan runs when its view is `all`,
  as it does for a `MEMBER`;
* the plan's atom accessor returns its atom, as it does an `ANTI_MEMBER`'s,
  so that the evaluation's count of operations that read an `old` view,
  which skips a variant while every `old` view it reads is empty, counts
  a check of `old` as it counts a lookup of it;
* it holds no expression, so a plan's count of computed values and of
  calls is unchanged by it;
* it lets a binding through at most once, so the streaming decision
  treats it as a test;
* `-e` prints it, section 7.

**A round cuts a plan whose first operation is a `SCAN`** into pieces of
its view, `specs/design-parallel.md`, and leaves any other plan whole. A
plan that begins with a literal binding nothing and reading nothing bound,
`p(_)` first, begins with a `SCAN` under `-X no-exists` and with an
`EXISTS` otherwise, and is cut under the one and whole under the other.
The whole plan is a probe followed by the rest of the body, whose first
generator is not the first operation; it runs on one thread, as every plan
beginning with a `LOOKUP` or a `MEMBER` does, and the piece machinery is
reached only through the question of whether a plan is cut, so no
assertion of it sees the check.

**More query plans stream.** A plan is distinct when no generator has a
wildcard column and its `EMIT` reads every slot a generator binds,
`specs/design-comparison-cost.md` section 4.1; the argument rests on tests
letting a binding through at most once, which an `EXISTS` does. A query
whose plan places a check after the generator that binds its key, as
`q(X), p(_, X)?` is planned when `q` is placed first, is distinct and
streams, where the `LOOKUP` with a wildcard that `-X no-exists` plans
there gathers. A query whose plan binds a slot from a generator with a
wildcard, as `p(_, X)?` must, gathers either way, section 11.

---

## 7. Explain

An existence check prints as its kind, the view when it is `delta` or
`old`, the atom and, when the key names a secondary index, the key, in the
form an `anti-member` prints:

```text
rule s(X) :- p(_, X), p(_, X), p(_, X).
    scan p(_, X)
    exists p(_, X) key={1}
    exists p(_, X) key={1}
    emit s(X)
```

The key is printed only for the partial shape, since the atom shows which
columns are `ANY`, and a partial check planned without indexes prints
without one, as a lookup planned without indexes prints as a scan. A
`delta` or `old` view is written after the kind as it is for every
operation over a member: `exists delta p(X, _)`.

**A dead variable prints as `_`**, in a check and in a generator alike.
The line shows what the plan reads, and the plan reads nothing in that
column; the `rule` line above it shows the name the author gave.
`scan e(X, _)` under `rule p(X) :- e(X, _Y).` tells a reader both that
`_Y` is never used and that the plan does not pay for it, which is what
`-e` is for.

Every line stays printable ASCII and within the limit of
`specs/design-planner.md` section 8.3, since nothing new is formatted.

---

## 8. The reference

`-X no-exists`, `RELIC_F_NO_EXISTS`, plans every positive literal by the
rule of `specs/design-plan.md` section 5 alone: the dead mask is empty, a
wildcard keeps its literal a generator, a named dead variable is bound by
its literal, and a literal binding nothing but holding a wildcard is a
`SCAN` or a `LOOKUP` with an `ANY` term. It joins the table of
`specs/design-planner.md` section 6:

| Switch | Flag | Effect |
| --- | --- | --- |
| `-X no-exists` | `RELIC_F_NO_EXISTS` | no `EXISTS`: a literal that binds nothing anything reads is a generator, and continues the join once per tuple it matches |

It leaves everything else as it is, including `MEMBER` for a literal whose
every column is bound, which is not a choice this document made.
Architecture section 66.2 gains the equivalence

```text
existence(P)           ==  no-exists(P)
```

The order does not depend on the flag, section 5, and the bound sets of
the two sides differ only in dead slots, which nothing reads; so the two
sides hold the same plans but for the operations this document changes
and the `_` it writes for a named dead variable. A check succeeds exactly
when the generator it replaces would succeed at least once, and the slots
the generator would bind are read by nothing, so the two derive the same
tuples; the check derives each of them fewer times, which the
`derivations` counter shows and every other total does not.

---

## 9. Tests

* **The regression workload**, in `tests/perf.sh` beside the planning
  workloads of `specs/design-planning-cost.md` section 8: `s(X)` with 48
  and then 64 calls of `p(_, ..., X, ...)` over two facts of `p`, answered
  from the whole program, each within the CPU budget the other workloads
  run under and answering one row. It passes only with the operation,
  which is what makes it the regression test; a run under `-X no-exists`
  is not budgeted.
* **Plan unit tests** in `tests/unit/test_plan.c`, beside the ones that
  show a fully bound literal is a membership test, checking the terms and
  not the kind alone: a literal whose unbound variables are all dead is an
  `EXISTS` keyed on its bound columns with `ANY` in the rest; a named dead
  variable and `_` give the same plan; `p(Y, Y)` with `Y` held nowhere
  else is a generator whose second column is `BOUND` and whose key is
  empty; `Y = 3, p(X, Y)` keeps column 1 a key column; a literal placed
  last that binds only what the head does not read is a check; a variant
  whose delta literal is a check in the placed plan and a generator in the
  delta-first one; a check of a delta begins at the delta's start; a rule
  the Magic rewrite generated is planned from its own mentions; a variable
  shared with an aggregate's body is not dead; and under `-X no-exists`
  every plan is the one the reference planner builds.
* **Golden plans** under `tests/lang`: every `-e` test whose plan held a
  generator with a wildcard after its bound columns changes that line to
  `exists`, every generator line holding a named dead variable writes `_`
  for it, every `-X stats` expectation over a rule with a check changes
  its `derivations`, and a new test shows each shape -- partial with its
  key, partial under `-X no-index` without one, empty, `delta` and `old`
  -- and a query that streams because its last literal is a check, with
  `-X no-stream` showing the same rows.
* **Equivalence** under `tests/equiv/strategies.sh`: `no-exists` joins the
  strategies that evaluate the same rules in the same order, held to the
  default's answers, diagnostics and tuple totals, and `no-exists+no-magic`
  those answering from the whole program. Under `tests/random`,
  `no-exists` and `no-exists+no-magic` join the strategy table with the
  default and `no-magic` as the references their totals are held to. The
  harness also expects of materializing a program one derivation per
  distinct binding of each rule body's variables, and skips that
  expectation for a program with `_` in a positive literal; it skips it
  for a program with a named dead variable in one as well, since the
  count a check makes depends on whether the literal holding the variable
  is a check or a generator, which the order decides, and no formula over
  the bindings gives it. The equivalence of answers and totals is what
  the strategies check. The naive fixed point of D-9 runs the same plans
  and is a check of the executor's new case rather than of the planner's.
* **Interruption**: the partial shape under `-X no-index` is one more
  unresumable scan, and the test in `tests/unit/test_exec.c` that
  `specs/design-interrupt.md` section 6 lists for consecutive negated
  probes scanning past a quantum is run against positive ones.

---

## 10. Determinism

* The dead mask is a function of the rule.
* The operation a literal becomes is a function of its columns, the bound
  set and the mask, each of which is a function of the source and the
  statistics the order was.
* A check reads the index a lookup on the same columns reads, keyed by the
  same mask and built in the same order.
* `-e` lines are a function of the plans.

So two runs over the same input produce the same plans and byte-identical
`-e` output, as `specs/design-planner.md` section 9 promises, and the
plans with and without the operation differ only where section 8 says.

---

## 11. What is deliberately absent

* **A generator that stops after its first match.** A `once` flag on a
  `SCAN` or a `LOOKUP` would answer the same question. But a generator
  that has matched and is resumed must reject while one that paused
  mid-walk without matching must continue, so the flag needs a
  matched-already bit in the operation's state to survive a quantum pause,
  and the loop would carry a generator that is sometimes a test. The probe
  exists, answers in one read, and is a test by construction.
* **Projecting a generator's dead columns away.** `s(X) :- r(X, _)` binds
  `X` once per tuple of `r`, duplicates included: the generator binds a
  slot something reads, and the duplicates are at most the relation's
  size. Deriving each live binding once would mean hashing the bindings
  seen, memory of the relation's order per generator, to remove a cost
  linear in the relation. Section 2's cost was exponential in the rule,
  and it came from the literals after such a generator, which are checks.
* **A cost model for tests.** A check is scored by the tuples that agree
  with its key and multiplies a variant's prefix and fan-out by that
  number, as a `MEMBER` is and does, though either passes a binding on at
  most once. Capping a test's estimate at one, and ranking a literal that
  would be a check ahead of one that would not, are each a change to the
  estimate of `specs/design-planner.md` section 4 that would move orders
  and thresholds the golden plans record, for `MEMBER` as much as for this
  operation. They are a question for the cost model, asked of both
  shapes at once, and not for the operation.
* **Folding `MEMBER` into `EXISTS`.** They are one operation with a full
  and a partial key, as an `ANTI_MEMBER` is with its three shapes. Keeping
  the name keeps every `member` line of the golden plans, the vocabulary
  of the documents and the executor's canonical-index path, for the price
  of one more arm in each switch.
* **An order-dependent liveness.** A pass over a decided order could mark
  a binding dead when nothing placed after it reads it. Section 3 shows
  that set is the dead variables, so the pass would compute from the order
  what the rule already says, and would make the bound set of the order
  differ from the plan's.
* **A limit on evaluation time.** The sentence in `relic.3` stands. A
  program can still write a join that runs for as long as it likes by
  reading what it binds; this document removes a cost paid for not reading
  it.

---

## 12. Amendments this design makes

On acceptance the durable record changes in these places, and nowhere
else:

* `specs/design-plan.md` section 3: the enum, the families and the table
  gain `EXISTS`; section 4: the invariant names it among the operations
  `ANY` occurs in; section 5: the first case reads "a positive literal
  that binds nothing", `MEMBER` when every column is bound and `EXISTS`
  otherwise, a dead variable binding nothing; sections 5.2 and 5.3: the
  delta literal's list of operations gains `EXISTS`.
* `specs/design-planner.md` section 5.1: a literal binds its variables
  that are not dead; section 5.2: the access path of a literal that binds
  nothing, and the delta literal's list of operations; section 6: the
  table gains `-X no-exists`; section 8.2: the `exists` line and the `_`
  a dead variable prints as.
* `specs/design-evaluation-cost.md` section 3.3: an `EXISTS` reads a
  member through its view as a `MEMBER` does, so the operations that never
  read a member are `ANTI_MEMBER`, `FILTER`, `ASSIGN` and `AGGREGATE`;
  section 4.3: the delta literal's list of operations.
* `specs/design-negation.md` section 4: the probe reads through a view,
  of which `all` is the negated literal's case.
* `specs/design-index.md` section 4.1: a check of a delta walks the
  frontier as a lookup of it does, and the argument for concurrent walks
  at one start covers both.
* `specs/design-interrupt.md` sections 4 and 6: the partial check under
  `-X no-index` among the unresumable probes.
* `specs/design-comparison-cost.md` section 4.1: a wildcard is a
  generator's column or a check's, and the syntactic reading of a distinct
  plan admits a `_` or a dead variable in a literal that is a check.
* `specs/architecture.md` section 58 shows the operation and points here;
  section 66.2 lists the equivalence.
* `lib/internal.h`: the contract of the helper that says what a literal
  binds, which Magic's replay reads as the slots of the literal's
  variables and the planner reads less the dead mask, says so; the
  comment at the replay that follows it says the same.
* `include/relic.h`: `RELIC_F_NO_EXISTS`, the next flag bit; `man/relic.1`
  for `-X no-exists` and `man/relic.3` for the flag; `NEWS`.
* `TODO.md` follows the implementation.
