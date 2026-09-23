# relic Magic Sets design

**Status.** Design document for the Magic Sets rewrite: how a query that binds
some of its arguments is turned into a program that derives only what the
query can reach, how that program is evaluated by the existing engine, and how
the rewrite is shown to a person. Forward-looking, like every document under
`specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 32 to 39, 60, 61 and 63;
`specs/design-plan.md` for the query rule this rewrites;
`specs/design-planner.md` for the ordering the rewrite borrows;
`specs/design-stratify.md` for the analysis the rewritten rules pass through;
`specs/design-eval.md` for the fixed points that run them; `relic-plan.md`
decisions D-4, D-7, D-8 and D-12.

---

## 1. What this document settles

* which predicates a query reads completely and which it reads on demand;
* what an adornment is, what an adorned predicate and a magic predicate are
  called, and which adornments have no magic predicate;
* the order in which bindings flow through a rule body, and why it is the
  planner's;
* the rules the rewrite generates, the seed among them, and the rewritten
  query;
* why the rewrite terminates, and how duplicate rules are recognized;
* the limits on what the rewrite generates, and what their diagnostics say;
* how the rewritten program is evaluated, what it does to the relations the
  context holds, and when a query is not rewritten at all;
* why the answers are the answers full materialization gives;
* the `-e` text of a rewrite.

---

## 2. Where the rewrite sits

Architecture section 39 defines two modes. Full materialization evaluates the
whole program and bypasses Magic Sets. Query-directed evaluation, the default
for a query, rewrites it first:

```text
query -> complete part materialized -> rewrite -> stratify -> evaluate -> answer
```

A query is **not** rewritten when the context's relations already hold the
program's fixed point -- `relic_materialize()` ran, or `-a` was given, and
nothing has been loaded since. Everything the query could read is then
complete, and running its plan over those relations, as a query always did
before this stage, is cheaper than any rewrite. Nor is it rewritten under
`-X no-magic` (`RELIC_F_NO_MAGIC`), which materializes the program and runs the
plan directly: that is the reference the rewrite is tested against.

Otherwise the query is answered in two phases over relations emptied for it:

1. **The complete part** is materialized: the program components the query
   reads in full, evaluated exactly as full materialization evaluates them,
   in schedule order. Section 3 says which they are.
2. **The demanded part** is rewritten: every other predicate the query
   reaches is replaced by adorned copies guarded by magic relations, planned
   against the relations phase 1 made complete, and evaluated by the same
   engine. The rewritten query then runs once over the result.

There is no second execution path. The rewrite produces ordinary compiled
rules; the analysis of `specs/design-stratify.md` schedules them, the planner
plans them, and the fixed points of `specs/design-eval.md` run them.

---

## 3. What a query reads completely

A predicate the query reaches is read either **completely**, from a relation
holding every tuple the program derives for it, or **on demand**, through an
adorned copy that holds only the tuples some caller asked about.

A predicate is read completely when any of these holds:

* **it is extensional**: every rule deriving it is a fact, or nothing derives
  it at all. Adorning it would copy each fact into a guarded rule per
  adornment, a million rules for a million facts, to compute a relation that
  loading the facts computes directly;
* **the query reaches it through negation**. Architecture section 37 keeps
  negation simple by materializing a negated relation in full in its lower
  stratum, so that absence means absence;
* **something read completely depends on it**, positively or negatively. A
  relation is complete only if everything it is derived from is.

Everything else the query reaches through positive literals is read on demand.

The set is computed by one walk over the program's schedule, before anything
is evaluated:

```text
demanded = {}, roots = {}
visit the query's body:
	a positive literal over an extensional predicate: add it to roots
	a positive literal over any other predicate p: if p is not in
	    demanded, add it and visit the body of every rule deriving p
	a negated literal over q: add q to roots
complete = roots and everything they depend on
read on demand = demanded minus complete
```

The walk visits the rules of a predicate that turns out to be complete as well
as the rest. That is harmless: whatever it finds beyond such a predicate is
something the predicate depends on, and so is complete too. The closure is
taken over components of the schedule, since a component's members depend on
one another, so the complete part is a set of whole components, and phase 1 is
the schedule filtered to them.

Two consequences are worth stating. A predicate that is both negated
somewhere and called positively elsewhere is read completely everywhere, since
it is materialized anyway. And a program whose relations the query never
reaches is never evaluated: facts the query cannot see are not loaded.

---

## 4. Adornments and names

An **adornment** says, for each argument of a literal at the point the literal
is evaluated, whether its value is already known: `b` for bound -- a constant,
or a variable bound earlier -- and `f` for free. A wildcard is always free.
Internally it is a column mask, bit *c* set when column *c* is bound, which is
the shape an index key already has.

Each predicate read on demand is adorned once per adornment it is called
with, and each adornment is a separate predicate, per architecture section 33.
The names follow D-12, built from the predicate and the adornment and never
from a counter, and contain `$`, which no identifier the lexer accepts can,
so they collide with nothing a program writes:

| Predicate | Name | Arity |
| --- | --- | --- |
| `path/2` called with its first argument bound | `path$bf` | 2 |
| the demand for it: the bound arguments asked about | `magic$path$bf` | 1 |

A predicate of arity zero is adorned as `p$`.

**An adornment with no bound column has no magic predicate.** A magic relation
records which bound values were asked about, and with nothing bound there is
nothing to record: its only possible tuple is the empty one, and it would
exist exactly when the adorned predicate is reached at all, which the rewrite
already knows. `path$ff` is therefore a copy of `path` with no guard. It is
still a copy, rather than `path` itself, because its rules are rewritten too:
a bound call made inside one of them propagates demand like any other.

Generated predicates enter the context's predicate table like any other and
are never removed, per `specs/design-program.md` section 3. Their number is
bounded by the adornments the program admits, and a second query with the
same adornments reuses them. They are marked defined when interned, so the
undefined-predicate warning never names one.

---

## 5. Sideways information passing

The rewrite needs, for each rule and each adornment of its head, the order in
which the body's literals bind their variables: a literal's adornment is what
the literals before it have bound. Architecture section 35 requires that this
order be the planner's, so that the rewrite has no private notion of body
order.

The planner's ordering is therefore split from the plan it emits:

```c
int	relic_plan_order(relic_t *, relic_arena_t *, const relic_rule_t *,
	    uint64_t bound, const bool *incomplete, size_t *order);
```

It fills `order` with the body indexes of every literal in the order the plan
would place them -- positive literals greedily, tests at the earliest point
their slots are bound -- starting from the slots in `bound`. Planning a rule
for evaluation is the same ordering from nothing bound, followed by emitting
one operation per literal in that order. One function decides the order for
both uses, and nothing else does.

For the rewrite the initial bound set is the variables of the head's bound
columns, and the planner runs in the static mode of `specs/design-planner.md`
section 7. The relations it can measure are the complete ones, which phase 1
has just filled: their tuple counts and distinct values are real. The
relations it cannot are those read on demand, marked incomplete, and estimated
by the documented constants. This is the reason for the two phases: planning
the rewrite before the complete part is loaded would measure every relation
as empty, and an empty relation estimates zero and is placed first.

A bad order costs demand precision, never correctness: every order gives the
same answers, as section 9 shows. Under `-X no-reorder` the order is the
written one, which makes the textual rewrite available as a reference too.

---

## 6. The generated rules

Take a predicate *p* adorned with *α*, and a rule deriving *p*, its body
ordered for *α* as section 5 describes. The rewrite generates:

* **the adorned rule**: the rule with *p* renamed `p$α`, the guard
  `magic$p$α(...)` over the head's bound arguments placed first, and every
  positive literal over a predicate read on demand renamed to the adorned
  predicate for its own adornment. Literals over complete predicates,
  negated literals and comparisons are kept as they are. The body is written
  in the section 5 order, guard first. With no bound column in *α* there is
  no guard;
* **one magic rule per positive literal over a predicate read on demand**,
  whose adornment *β* binds at least one column: its head is
  `magic$q$β(...)` over the literal's bound arguments, and its body is the
  guard followed by every literal placed before it in the order. The magic
  rule says: whenever the rule is called with these bound values, and the
  literals before this one hold, this literal is called with these.

A fact of a predicate read on demand is a rule with an empty body, and is
rewritten like one: `path(a, b).` adorned `bf` becomes
`path$bf(a, b) :- magic$path$bf(a).`

**The query is a rule too**, whose head is its output columns and names no
predicate (`specs/design-query.md` section 4), and is rewritten with nothing
bound and no guard; the rewritten rule keeps that head and runs into the
query's own answer relation. The magic rules its body generates are the
**seeds**: with no guard and nothing before the first literal, the first seed
of `path(a, X)?` is the fact `magic$path$bf(a).`, exactly architecture section
34's seed, found by the one rule rather than constructed separately. The
rewritten query, `$q0(X) :- path$bf(a, X).`, is kept apart from the generated
rules and run once after them, as a query always is.

For the graph program of architecture section 36, `path(a, X)?` generates:

```prolog
magic$path$bf(a).
path$bf(X, Y) :- magic$path$bf(X), edge(X, Y).
path$bf(X, Z) :- magic$path$bf(X), edge(X, Y), path$bf(Y, Z).
magic$path$bf(Y) :- magic$path$bf(X), edge(X, Y).
```

The magic relation is the set of nodes reachable from `a`, and `path$bf` holds
paths starting at those nodes only.

### 6.1 A generated rule is a compiled rule

Generated rules are ordinary `relic_rule_t` values, so nothing downstream
distinguishes them. Their variables are **renumbered** in order of first
occurrence, head then body, and carry the names of the variables they came
from. Renumbering keeps the invariant of `specs/design-program.md` section 4 --
every slot below the variable count is bound by a positive literal -- which a
magic rule would otherwise break by leaving out variables of its source rule;
and it gives each rule a canonical form, which is what section 7 compares.

Every generated rule is safe by construction. A magic rule's head reads only
columns its adornment calls bound, which the guard or a literal before it
binds, and a test is placed only where its slots are bound. An adorned rule
reads the variables its source rule did, through the same positive literals.

---

## 7. Termination and duplicates

**The worklist is memoized on the predicate and the adornment**, per
architecture section 38. Adorning `(p, α)` for the first time records it and
queues the rules of *p*; asking again finds the record. Each record is
processed once, so the rewrite ends when every adornment it has recorded has
been processed. There are at most 2^arity adornments of a predicate -- finite,
since arity is at most 32, but not small -- so the practical bound is the
generated-predicate limit, or budget, of section 8.

The records are kept in the order they were first asked for, and processing
walks them in that order, which is the order `-e` lists them in.

**Duplicate rules are dropped.** Two source rules that share a prefix generate
the same magic rule, up to the names of their variables:

```prolog
p(X) :- a(X), q(X).
p(Y) :- a(Y), q(Y), s(Y).
```

both give `magic$q$b(X) :- magic$p$b(X), a(X).` after renumbering. A
generated rule is compared structurally with those already generated, through
a hash table over their canonical forms, and a duplicate is not appended.
Renumbering is what makes a plain structural comparison enough: two rules
equal up to renaming have equal canonical forms.

**A rule whose head is one of its own positive literals is dropped.** It can
only derive a tuple that the relation already holds. It arises naturally:
`p(X, Y) :- p(X, Z), e(Z, Y).` adorned `bf` generates
`magic$p$bf(X) :- magic$p$bf(X).`, which would otherwise make a recursive
component out of nothing.

---

## 8. Limits and budgets

Architecture section 63 and D-7 make two limits configurable, and counted per
rewrite:

| Limit | Counts | Checked |
| --- | --- | --- |
| `max_magic_preds`, `-l magic-preds=N` | adorned and magic predicates generated | before one is interned |
| `max_magic_rules`, `-l magic-rules=N` | rules generated, after dropping duplicates | before one is appended |

The diagnostic has the form every limit uses and names the predicate whose
adornment exceeded it -- the one that exploded, which is what the reader has
to change:

```text
relic: limit exceeded: magic-preds=64 while adorning chain/3
relic: limit exceeded: magic-rules=1000 while adorning path/2
```

A generated rule is attributed to the source predicate its head belongs to:
an adorned rule to the predicate adorned, and a magic rule to the predicate
whose demand it records, so a seed is attributed to the predicate the query
calls. Exceeding a limit fails the query as any limit does: no rows, and a
nonzero exit.

**A limit left unset is a budget.** Finite is not small: a program whose
rules each call a predicate with one more column bound than the head was
called with reaches all 2^arity adornments. Three kilobytes of arity 14 do
it, generating 16 384 adornments in 1.3 GB, where the query answered from the
whole program takes a megabyte. The rewrite is an optimization, and an
optimization must not decide whether a query is answered, nor generate a
program unboundedly larger than the one it specializes. So a Magic limit that
is not configured is replaced by a budget in proportion to the program:

| Budget | Counts | Is |
| --- | --- | --- |
| generated predicates | as `max_magic_preds` | 256 + 16 x the program's derived predicates |
| generated rules | as `max_magic_rules` | 256 + 16 x the program's rules |
| generated literals | body literals of generated rules, after dropping duplicates | 256 + 16 x the program's body literals |

A program's derived predicates are the members of its schedule, its rules
are its compiled rules, facts included, and its body literals are the
literals of those rules' bodies. All are fixed for a given program, so the
budget is the same on every run. The literal budget has no configurable
limit behind it and applies whether or not the other two are configured: it
is not a knob but the invariant that the rewrite is a constant multiple of
the program, which the rule budget alone cannot hold, because a magic rule's
body is the prefix before its call and a body of many calls generates
prefixes whose total is the square of its length
(`specs/design-planning-cost.md` section 2). The factor admits every rewrite that
specializes the program -- a handful of adornments per predicate, each with
its rules and magic rules -- and the floor admits every rewrite of a small
program outright; what it refuses is a rewrite that multiplies the program.
The budgets count rules, so they bound the rewritten program's size only as
far as a rule's length is bounded: each adornment copies every rule of its
predicate whole, and generates a magic rule per demand call whose body is
the prefix before it. What that costs, and the memory limit that bounds it,
is `specs/design-planning-cost.md`.

Outgrowing a budget abandons the rewrite. A warning names the budget and the
predicate being adorned, and the query is then answered from the whole
program's fixed point, exactly as `-X no-magic` answers it:

```text
relic: warning: the query's Magic rewrite outgrew its budget of 416 generated rules while adorning p/8; answering it from the whole program
```

A configured limit replaces the budget rather than joining it, so a caller
who sets one gets precisely the limit asked for: a larger rewrite than the
budget admits if the limit is larger, and a failed query when it is
exceeded.

The budget bounds the rewritten program, not what it derives. Adornments of
one predicate with different bound columns each derive their own relation,
and on a program whose calls bind many patterns those relations together can
hold several times the tuples of the whole program's fixed point, though never
more rules than the budget admits.

---

## 9. Evaluating the rewrite

### 9.1 Stratification is a guard

The generated rules are analysed by the same dependency analysis as the
program, into a schedule of their own. That is the check `relic-plan.md` asks
for: a rewrite that produced recursion through negation would be caught there
rather than evaluated into nonsense.

By construction it never does. Every negated literal in a generated rule reads
a complete predicate, which no generated rule derives, so a negative edge
always leaves the generated rules and can lie on no cycle. The rewrite checks
the first half as it writes each body: a negated literal over a predicate read
on demand would read an empty relation and succeed, so it fails the query with
an internal error instead. Should the analysis
nevertheless find one, it reports the cycle as it would for a program, and the
query fails with an internal error.

### 9.2 State

A rewritten query **starts from empty relations**. It discards every relation
first, as a materialization with stale relations does, and leaves behind the
complete part and the generated relations; the next evaluation of any kind
discards them in turn, and the answer is emptied when its rows have been
delivered. What the context holds after a query is then a
function of that query alone, so its counters and its memory are the same
whatever ran before it. A query whose rewrite is abandoned over its budget leaves
the whole program's fixed point instead, as a query under `-X no-magic`
does. The price is that each rewritten query loads the
facts it reads again. `TODO.md` records keeping complete relations across
queries as an optimization.

The discard runs before either phase begins, so no delta range spans it --
the obligation `specs/design-relations.md` section 4 places on any operation
that ends a span of the append-only store.

### 9.3 Why the answers are right

**Sound.** Every generated rule is a source rule with literals renamed and,
for an adorned rule, a guard added; or a magic rule, whose head no answer
reads except through a guard. By induction on derivations, every tuple of
`p$α` is a tuple of *p* in the program's least model: a guard only restricts,
an adorned literal holds a subset of its predicate by induction, and a
complete literal, negated or not, reads the relation full materialization
computes, because phase 1 computed it the same way. So every answer is an
answer of full materialization.

**Complete.** Claim: if `p(t)` is in the least model, and either *α* binds no
column or `magic$p$α` holds the bound part of *t*, then `p$α(t)` is derived.
By induction on the height of a derivation of `p(t)`: take the rule instance
at its root, and walk its body in the section 5 order. At each literal over a
predicate read on demand, with adornment *β*, the magic rule for that literal
fires -- its guard holds by assumption, and every literal before it holds:
complete ones and tests because the least model agrees, adorned ones by the
induction hypothesis, their derivations being shorter and their own demand
established earlier in the walk. So `magic$q$β` holds the bound part of that
literal's tuple, and by the hypothesis the adorned literal holds too. Every
literal of the adorned rule then holds, and it derives `p$α(t)`. Applied to the
query, whose rewrite has no guard, this gives every answer.

The equivalence `magic(Q, P) == Q(full_materialize(P))` is what the test suite
checks, over the language corpus and under every strategy.

---

## 10. Explain

With an explain callback installed, a rewritten query shows, in the order it
runs: the program components of phase 1, headed as in full materialization; a
heading for the rewrite, listing the adornments in the order recorded and the
seeds; the components of the rewritten program, nested one level beneath it,
with their own strata and positions; and the rewritten query's plan. For
architecture section 60's program, a chain of three edges, and
`path(a, X)?`:

```text
stratum 0, component 0: edge/2
    facts: 3
magic rewrite of $q0(X) :- path(a, X).
    adorn path/2 bf
    seed magic$path$bf(a).
    stratum 0, component 0 (recursive): magic$path$bf/1
        facts: 1
        rule magic$path$bf(Y) :- magic$path$bf(X), edge(X, Y).
            scan edge(X, Y)
            member magic$path$bf(X)
            emit magic$path$bf(Y)
            delta magic$path$bf(X):
                scan delta magic$path$bf(X)
                lookup edge(X, Y) key={0}
                emit magic$path$bf(Y)
    stratum 0, component 1 (recursive): path$bf/2
        rule path$bf(X, Y) :- magic$path$bf(X), edge(X, Y).
            scan edge(X, Y)
            member magic$path$bf(X)
            emit path$bf(X, Y)
        rule path$bf(X, Z) :- magic$path$bf(X), edge(X, Y), path$bf(Y, Z).
            scan edge(X, Y)
            member magic$path$bf(X)
            lookup path$bf(Y, Z) key={0}
            emit path$bf(X, Z)
            delta path$bf(Y, Z):
                scan delta path$bf(Y, Z)
                lookup edge(X, Y) key={1}
                member magic$path$bf(X)
                emit path$bf(X, Z)
query $q0(X) :- path$bf(a, X).
    lookup path$bf(a, X) key={0}
    emit $q0(X)
```

* The phase 1 components keep their positions in the program's schedule, so
  a component the query does not read leaves a gap in the numbering, and the
  gap is what shows that it was not evaluated.
* `adorn` names the source predicate and the adornment; the adorned and magic
  names follow from them by section 4.
* `seed` shows each rule the query's body generated, fact or not. Facts in a
  component are counted, as everywhere, and a seed that is a fact would
  otherwise be visible only as a count.
* A query whose body reads only complete predicates has nothing to adorn and
  shows no rewrite heading: its phase 1 components and its plan.

The lines follow the rules of `specs/design-planner.md` section 8.3: printable
ASCII, escaped constants, cut at 1023 bytes.

---

## 11. Memory

The rewrite lives for one query run. Generated rules are held in a growable
rule table, with their argument and body arrays and the scratch space for
building names in an arena of the rewrite's own; the memo and duplicate
tables are identifier tables over the rewrite's arrays. All of it is released
when the query has run, successfully or not, as is the schedule of the
rewritten program. Symbol and predicate entries for generated names belong to
the context, as every interned entry does.

The rewrite's arena is charged to `max_memory`, per
`specs/design-planning-cost.md` section 6: the budgets of section 8 count
rules, and a rewrite's size in literals is the square of a body's length,
which no budget bounds. The growable tables beside the arena are linear in
the rules the budget admits and are not charged.

---

## 12. Determinism

* The complete part is found by a walk whose order depends only on the
  program and the query, and evaluated in schedule order.
* Adornments are recorded in the order they are first asked for, and
  processed in that order; rules are generated in program order and, within
  a rule, its magic rules in body order and then the adorned rule.
* The order within a body is the planner's, which is deterministic per
  `specs/design-planner.md` section 9.
* Names derive from the predicate and the adornment.
* Duplicates are found by comparison, not by address.

So the same program and query give the same generated rules in the same order,
the same schedule, and byte-identical `-e` output.

---

## 13. What is deliberately absent

* **Demand through negation.** Architecture section 37: a negated relation is
  materialized in full. Propagating bindings into it would need the
  refinements of magic sets for stratified negation, which are correct only
  under conditions that are not obvious on first reading.
* **Adorning extensional predicates.** Section 3.
* **Supplementary magic predicates.** They save recomputing a shared prefix
  between an adorned rule and its magic rules at the cost of another relation
  per literal; the prefixes here are short, and a measurement should come
  first.
* **Relations kept across queries.** Section 9.2.
* **A rewrite of a program already materialized.** Section 2.
