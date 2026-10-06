# relic Magic Sets design

**Status.** Design document for the Magic Sets rewrite: how a query that binds
some of its arguments is turned into a program that derives only what the
query can reach, how that program is evaluated by the existing engine, and how
the rewrite is shown to a person. Forward-looking, like every document under
`specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 32 to 39, 60, 61 and 63;
`specs/design-plan.md` for the query rule this rewrites;
`specs/design-planner.md` for the ordering the rewrite borrows;
`specs/design-stratify.md` for the analysis the rewritten rules pass through;
`specs/design-eval.md` for the fixed points that run them;
`specs/design-facts.md` for the base stores a rewrite reads facts from;
`specs/decisions.md` D-4, D-7, D-8 and D-12.

---

## 1. What this document settles

* which predicates a query reads completely and which it reads on demand;
* what an adornment is, what an adorned predicate and a magic predicate are
  called, which adornments have no magic predicate, and which a free
  adornment subsumes;
* the order in which bindings flow through a rule body, and why it is the
  planner's;
* the rules the rewrite generates, the seed among them, the rule that reads
  a derived predicate's facts through its base view, the supplementary
  predicates that carry a prefix from one call to the next, and the
  rewritten query;
* why the rewrite terminates, and how duplicate rules are recognized;
* the limit on the work the rewrite may do, and what its diagnostic says;
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
the program has gained no rule and no fact since. Everything the query could
read is then complete, and running its plan over those relations is
cheaper than any rewrite. Nor is it
rewritten under `-X no-magic` (`RELIC_F_NO_MAGIC`), which materializes the
program and runs the plan directly: that is the reference the rewrite is
tested against.

Otherwise the query is answered in two phases over derived relations
emptied for it:

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

* **no rule derives it**: it is not derived (`specs/design-facts.md` section
  3.2), so its base store, filled when its facts were loaded, is the whole
  relation, and reading it needs no evaluation. A predicate of facts alone
  is one, and so is a base view (section 6);
* **the query reaches it through negation**. Architecture section 37 keeps
  negation simple by materializing a negated relation in full in its lower
  stratum, so that absence means absence;
* **the query reaches it through an aggregate**: the predicate an
  aggregate literal folds, a group predicate or the atom's own, and every
  predicate a literal of the aggregate's body names, since a fold needs
  the whole group, `specs/design-aggregates.md` section 11;
* **something read completely depends on it**, positively or negatively. A
  relation is complete only if everything it is derived from is;
* **the rewrite keeps only free adornments**: when no adornment the rewrite
  keeps binds a column, every predicate the rewrite adorned is read
  completely, and no rule is generated (section 4).

Everything else the query reaches through positive literals is read on demand.

The set is computed by one walk over the program's schedule, before anything
is evaluated:

```text
demanded = {}, roots = {}
visit the query's body:
	a positive literal over a predicate no rule derives: add it to roots
	a positive literal over any other predicate p: if p is not in
	    demanded, add it and visit the body of every rule deriving p
	a negated literal over q: add q to roots
	an aggregate literal: add the predicate it folds to roots, and add
	    every predicate a positive or negated literal of its body
	    names to roots
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
reaches is never evaluated: a component the query cannot see is neither
seeded with its facts nor run.

The fourth source is known only once the adornments are, so it is decided
after the walk, by the search of section 4. Its predicates are closed
over components exactly as the walk's are, and the components they add are
evaluated from the program's schedule after the walk's, before the query
rule runs; the query rule is then the one written, planned over the
program's relations as a query against a materialized program is. That
generalizes the case where the relations already hold the fixed point: a
rewrite whose adornments bind nothing would derive exactly what full
materialization of the reached components derives, so the program's own
relations are read instead.

**Base views are interned before the walk.** When the rewrite begins finding
what the query reads completely, it interns the base view of every derived
predicate whose base store holds facts, before it takes its snapshot of the
predicate table, so every base view is inside the snapshot and complete by
the rule above. The rewrite asks whether a predicate is complete only of the
program's predicates and of base views, all of which that snapshot holds.

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
| the prefix before the second call binding a column of the second rule deriving `path$bf` (section 6) | `sup$path$bf$1$2` | its live variables |

A predicate of arity zero is adorned as `p$`. A supplementary predicate is
named by the adorned predicate, the position of its rule among the rules
deriving that predicate, counted from 0 in program order, and the position
of the call it precedes among the rule's calls binding a column, counted
from 1 in body order; one generated from the query's rule is named after
`$q0`.

**An adornment with no bound column has no magic predicate.** A magic relation
records which bound values were asked about, and with nothing bound there is
nothing to record: its only possible tuple is the empty one, and it would
exist exactly when the adorned predicate is reached at all, which the rewrite
already knows. `path$ff` is therefore a copy of `path` with no guard. It is
still a copy, rather than `path` itself, because its rules are rewritten too:
a bound call made inside one of them propagates demand like any other.

**A free adornment subsumes the others.** Once the query reaches a predicate
`p` with the adornment that binds no column, every call to `p` in every
generated rule, whatever columns the call binds, is a call to `p$ff`, and no
other adornment of `p` is generated. `p$ff` holds every tuple of `p` in the
program's least model, by the completeness of section 9.3 for an adornment
that binds nothing, and only such tuples, by its soundness. A call adorned
`bf` reads the tuples of `p` whose first column is the bound value, which
are the tuples of `p$ff` whose first column is that value: a lookup into
`p$ff` on that column, which the planner gives it as it gives any bound
literal. The redirected call binds the variables the adorned call bound, so
the literals after it see the same bound set and their adornments do not
change; it propagates no demand, so its magic rule is not generated. The
draft places its cuts by the columns calls bind, never by the adornments
they read (section 6), so the prefix is still cut before the redirected
call, and every rule drafted for an adornment is as long as it would be
without subsumption. Stratification stays a guard (section 9.1): the
redirection adds positive edges to `p$ff` and removes edges to `p$bf` and
its magic predicate, and a negated literal still names a predicate read
completely.

**Adorn first, draft second.** A bound adornment of `p` may be reached
before the free one, so the rewrite finds the adornments before it drafts
any rule. The search walks each rule's body once for the adornment of its
head: it puts the body in the order `relic_plan_order()` gives it, records
for each literal the variables bound before it, whether it calls a
predicate read on demand and which columns such a call binds, reaches the
adornment each call reads, and drafts nothing. The draft replays the walks
the search recorded, with the adornment each call reached, and orders no
body itself, so the search and the draft cannot disagree on the order of a
body, on the columns a call binds or on the adornment it reads. A predicate
is read free from the first call that reaches it free, and every later call
on it, whatever it binds, reaches the free adornment. An adornment of `p`
binding columns that an earlier call reached was reached on behalf of a
call the draft will not make, and so was whatever only it reached; when the
search kept one, it runs once more, with every predicate the first search
read free read free from the start. No third search is needed. Which
adornments a rule's calls reach depends on the adornment the rule is read
for and on which predicates are read free, and on nothing else, so every
call the second search makes the first made too, and every predicate it
reaches free is one the first read free already. Within one rewrite a walk
depends on the rule and the adornment alone -- which predicates are read on
demand, and the measures of those read completely, do not change while it
runs -- so the second search reads the walks the first recorded and walks
no rule again; only the adornment each call reaches is found afresh. The adornments the last search kept are named and drafted.
When none of them binds a column, the query reads every predicate the
search adorned completely, per section 3, and nothing is drafted.

When some adornment binds a column, a free copy `p$ff` stays a copy: its
rules may call a predicate `q` with a binding that nothing else asks for,
and that demand is worth propagating. The subsumption rule has already made
sure that no such call is to a predicate that is also reached free.

Generated predicates enter the context's predicate table like any other and
are never removed, per `specs/design-program.md` section 3. The adorned
and magic predicates are bounded by the adornments the program admits, and
a second query with the same adornments reuses them. A supplementary
predicate is named after its rule and the call it precedes, so their
number depends on the shape of the rules and the queries as well, within
the rewrite's limit. One cut from the query's rule is named after `$q0`
whatever the query, so a later query reuses it when its cut at that
position keeps as many variables, and otherwise gets a predicate of the
same name and another arity. No atom of a program or a
query names one, so none is ever referenced, and the undefined-predicate
warning never names one, whether or not the rewrite that generated it was
kept. A base view, `p$base`, is interned once, the first time a rewrite
needs it, and reused by every later one.

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
ordered for *α* as section 5 describes. Call a body literal a **demand
call** when it is a positive literal over a predicate read on demand whose
adornment *β* binds at least one column. The rewrite walks the body in that
order keeping a **prefix**, the literals evaluated so far as the generated
rules will write them. The prefix starts as the guard `magic$p$α(...)` over
the head's bound arguments, or empty when *α* binds no column. Each positive
literal over a predicate read on demand is appended renamed to the adorned
predicate for its own adornment; literals over complete predicates, negated
literals, comparisons and aggregate literals are appended as they are. The
rewrite generates:

* **one magic rule per demand call**, except a call that restates the head
  (section 7): its head is `magic$q$β(...)` over the call's bound arguments
  and its body is the prefix. The magic rule says: whenever the rule is
  called with these bound values, and the literals before this one hold,
  this literal is called with these;
* **one supplementary rule before every call binding a column but the
  first and those restating the head**, whether or not the call propagates
  demand, as a call the free adornment subsumes (section 4) does not: the
  prefix is not written out
  again but derived once, into a **supplementary predicate** whose columns
  are the prefix's *live* variables -- those it binds that occur in the
  call, in a literal after it, or in the head -- by the one rule whose body
  is the prefix. From there the prefix is that one literal: the call's
  magic rule, when it has one, every later rule and the adorned rule read
  the prefix through it;
* **the adorned rule**: the rule with *p* renamed `p$α` and the prefix after
  the last literal as its body.

A rule with one call binding a column, which most rules have, generates what
the plain construction does: the prefix before the call is written into its
magic rule, when it has one, and again into the adorned rule, and no
supplementary predicate exists. A rule with *k* calls binding a column over
*n* literals generates bodies holding about *n* + 2*k* literals rather than
the sum of the prefixes, about *nk*/2, and the prefix through a call is
evaluated once rather than once per later call and again for the adorned
rule. The cost is one relation per supplementary predicate, holding the
prefix's live bindings. For

```prolog
p(X, Z) :- p(X, Y), p(Y, Z).
```

as the second rule deriving *p*, adorned `bf`, the rewrite generates

```prolog
sup$p$bf$1$2(X, Y) :- magic$p$bf(X), p$bf(X, Y).
magic$p$bf(Y) :- sup$p$bf$1$2(X, Y).
p$bf(X, Z) :- sup$p$bf$1$2(X, Y), p$bf(Y, Z).
```

after dropping the first call's magic rule, `magic$p$bf(X) :- magic$p$bf(X)`,
per section 7; the supplementary relation holds the tuples of `p$bf`, so this
rule's memory doubles. The trade is made uniformly: a threshold on where to
cut would be a knob whose right value depends on the data, and what a
supplementary relation holds is counted by the tuple and memory limits as
every relation is.

A supplementary predicate is a predicate, of at most 32 columns per D-7,
while a rule has up to 64 variables. A cut whose live variables do not fit
is not made: the prefix continues through that call, written out in full in
the call's magic rule, to the next call where they fit or to the adorned
rule. Such a body is bounded as the plain construction is, by the limit of
section 8, which charges every prefix the draft copies.

**A derived predicate's facts enter each of its adornments as one rule over
its base view.** The facts a program states are tuples of their predicate's
base store, not rules (`specs/design-facts.md`), so the rewrite names the
base store of a derived predicate *p* as a predicate of its own, its **base
view** `p$base`, of *p*'s arity, which no rule derives and which reads *p*'s
base store. Each adornment of a derived predicate whose base store holds
facts is given, before the adornments of the program's rules deriving *p*,
the adornment of the rule `p(X1, ..., Xn) :- p$base(X1, ..., Xn).`:

```prolog
link$bf(X1, X2) :- magic$link$bf(X1), link$base(X1, X2).
```

rather than a guarded copy of each fact. The base view is complete, so the
planner looks the facts up by the bound columns like any complete relation,
the adornment reads only the facts its demand reaches, and the rewrite's
size does not depend on how many facts there are. The rule calls nothing on
demand, so it generates no magic rule, cuts no prefix and names no
supplementary predicate. Being first, it makes an adornment's rows begin
with its facts, as a derived predicate's rows do when it is materialized.

**The query is a rule too**, whose head is its output columns and names no
predicate (`specs/design-query.md` section 4), and is rewritten with nothing
bound and no guard; the rewritten rule keeps that head and runs into the
query's own answer relation. The magic rules its body generates are the
**seeds**: with no guard and nothing before the first literal, the first seed
of `path(a, X)?` is the body-less rule `magic$path$bf(a).`, exactly
architecture section 34's seed, found by the one rule rather than
constructed separately. A seed with no body is the only body-less rule a
rule table holds, since a program's facts are in base stores, and every
evaluation of the rewrite inserts it when its turn comes
(`specs/design-eval.md` section 3). The
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

**An aggregate literal is appended as it is**, `specs/design-aggregates.md`
section 11. The order of section 5 places it where its shared slots are
bound and reports that it binds its result slot, as it reports an
assignment's binding, so a call after it whose argument is the result is
adorned bound. The copy in a prefix has its shared and result slots
renumbered and keeps the predicate it folds, its key columns and its
compiled body; the folded predicate is read completely, section 3, and is
never adorned.

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
binds; a supplementary rule's head reads only variables its prefix binds; and
a test is placed only where its slots are bound. An adorned rule
reads the variables its source rule did, through the same positive literals.

A comparison is copied, renumbered, hashed and compared by walking its two
expression trees, and a call node is walked as an operator node is, with
its function and its arguments where the operator has its kind and its
operands. A copied `match` keeps its pointer to the pattern compiled with
its source rule, which lives in the program arena that every rewrite's
rules are outlived by; `specs/design-arithmetic.md` section 10 and
`specs/design-strings.md` section 10. The limit of section 8 counts a
comparison's nodes, so a call costs what its nodes do.

---

## 7. Termination and duplicates

**The worklist is memoized on the predicate and the adornment**, per
architecture section 38. Adorning `(p, α)` for the first time records it and
queues the rules of *p*; asking again finds the record. Each record is
processed once, so the rewrite ends when every adornment it has recorded has
been processed. There are at most 2^arity adornments of a predicate -- finite,
since arity is at most 32, but not small -- so the practical bound is the
limit of section 8, which charges the rules read for each adornment kept.

The records are kept in the order they were first asked for, and processing
walks them in that order, which is the order `-e` lists them in.

**Duplicate rules are dropped.** Two source rules that share a prefix generate
the same magic rule, up to the names of their variables:

```prolog
p(X) :- a(X), q(X).
p(Y) :- a(Y), q(Y), s(Y).
```

both give `magic$q$b(X) :- magic$p$b(X), a(X).` after renumbering. A
generated rule is compared structurally with those already generated,
through a hash table over their canonical forms, and a duplicate is not
appended. Renumbering is what makes a plain structural comparison enough:
two rules equal up to renaming have equal canonical forms. An aggregate
literal enters the canonical form by its function, the predicate it folds,
its key columns, its aggregated column and its slots,
`specs/design-aggregates.md` section 11.
Supplementary
predicates are named by their rule and call, so two source rules sharing a
prefix beyond their first call binding a column generate distinct
supplementary rules, and at most the magic rules before their first cut are
found equal. Sharing further would name a supplementary predicate by its
content, and what it would save is linear in what is kept.

**A rule whose head is one of its own positive literals is dropped.** It can
only derive a tuple that the relation already holds. It arises naturally:
`p(X, Y) :- p(X, Z), e(Z, Y).` adorned `bf` generates
`magic$p$bf(X) :- magic$p$bf(X).`, which would otherwise make a recursive
component out of nothing.

**A call that restates the head has no magic rule and no cut before it.**
A call on the head's predicate, binding the head's bound columns and no
others to the head's own arguments there, reads the adornment its rule is
rewritten for with the very tuple the guard holds. Its magic rule would
derive only that tuple; written over the uncut prefix, which starts with
the guard, it is dropped as above, but behind a cut it reads the
supplementary predicate rather than the guard, and the restatement is
hidden. In

```prolog
p(X, Y) :- q(X), p(X, Z), s(Z, Y).
```

as the first rule deriving *p*, adorned `bf`, with *q* derived by a rule and
so read on demand, the call on *p* is the second call binding a column, and
its magic rule would be
`magic$p$bf(X) :- sup$p$bf$0$2(X).`, deriving the magic and supplementary
relations from each other to no end. So the restating call records nothing,
and no cut is made before it: the cut would serve only the rules after the
call, which read the prefix written out instead up to the next call binding
a column, where it is cut as it would have been. The call still counts
among the calls binding a column, so the supplementary predicates after it
keep their names.

---

## 8. The limit on the rewrite's work

The rewrite does its work in two stages, and both do work in proportion to
what they read and write: the search finds the adornments the query reaches
by reading every rule of each adornment it keeps, and the draft generates
the rules of the adornments the last search kept. Both measure what they
handle in one unit, the size of a literal: one, and one more per node of
its expressions, so a literal carrying a large expression costs what
walking and copying its nodes costs (`specs/design-arithmetic.md` section
12). A rule's size is its body's plus one for the guard an adorned copy of
it is given. An aggregate literal is one literal wherever a literal is
counted: what it folds is read completely and its body is never copied,
`specs/design-aggregates.md` section 11. Facts count nothing: the rewrite
copies none of them, so a thousand facts behind one rule rewrite as cheaply
as one.

One limit bounds both stages, counted per rewrite:

| Limit | Counts | Default |
| --- | --- | --- |
| `max_magic`, `-l magic=N` | literals the rewrite reads and generates, search and draft together | 768 + 48 x the size of the program and the query |

**Everything the rewrite reads or generates is charged where it is read or
generated.** A search charges each rule it reads, its body's size, before
the rule is walked or its walk replayed, and every search charges what it
reads, so a second search is charged again for the walks of the first it
replays. The draft charges each rule it drafts, its body's size, before the
draft is canonicalized and compared with the rules kept, whether the rule is
then kept or dropped as a duplicate or as one that derives nothing: a
dropped rule is built before it is recognized as one, and a long body
calling its own predicate drafts copies that are all dropped, so a charge on
what is kept would never see the work of building and comparing them. The
query is charged as a rule is: its walk by each search that reads it and
its rewritten body by the draft. A call a search notes as subsumed by a free
adornment is a step of a walk already charged. Nothing else in the rewrite
loops over anything but the program, once, before the first search: mapping
its components, indexing its rules by head, interning its base views and
finding what the query reads completely are one pass each. Ordering a body
for its walk is the planner's work and costs what it costs the planner,
charged to the memory limit per D-18; the charge here counts the bodies
ordered. So the work the rewrite does after its first pass over the program
is a constant multiple of the literals it is charged, and the charge is a
measure of the work rather than of some output of it.

**One count bounds every part of the rewrite, by construction.**

* *Adornments.* Every adornment a search keeps is of a derived predicate,
  so the search reads at least one rule for it, and a rule has at least one
  body literal, a statement with none being a fact. So a search keeps no
  more adornments than the literals it is charged, and the rewrite generates
  at most two predicates per adornment, the adorned one and its magic
  predicate.
* *Supplementary predicates.* Each is the head of exactly one rule, drafted
  and charged with at least the prefix it cuts.
* *Rules.* Every rule the rewrite generates is charged its body before it is
  kept, and every one but a seed has at least one literal: an adorned rule
  its guard, a magic rule the literal it records a call from, a
  supplementary rule the prefix it cuts, a rule over a base view the view. A
  seed is the magic rule of a call the query makes, whose walk was charged,
  or of an adorned rule's first call when constants alone bind it, at most
  one per adorned rule, whose draft was charged. So the rules kept number at
  most twice the literals charged.
* *Working memory.* A search holds its adornments, kept and subsumed, and
  the walk of each rule it reads, a step of a few words per body literal,
  for the draft to replay. The walks grow with the literals read, are
  charged to the memory limit as the rest of the rewrite is, and are
  released once the rewrite has drafted. The planner's working state for
  one rule is released once the rule is walked or drafted, so a search holds
  no more than its adornments and their walks however many rules it reads,
  and the draft no more than the rules kept however many it drafts.

No count of predicates or rules is kept beside it: both are outputs, and
bounding the work through an output needs an argument that everything the
rewrite does yields some of that output, which is only ever as good as its
list of exceptions. There is no configuration in which a stage runs without
the count, because there is nothing to lift: a caller's value changes the
number and nothing else.

**Why the default is in proportion to the program.** Finite is not small: a
program whose rules each call a predicate with one more column bound than
the head was called with reaches all 2^arity adornments. Three kilobytes of
arity 14 do it, generating 16 384 adornments in 1.3 GB, where the query
answered from the whole program takes a megabyte. The rewrite is an
optimization, and an optimization must not decide whether a query is
answered, nor generate a program unboundedly larger than the one it
specializes. So the default is a floor of 768 literals plus 48 times the
size of the program and the query. The program's size is what one adorned
copy of it holds, each rule's body plus one for its guard; a derived
predicate whose base store holds facts counts as the one rule over its base
view that each of its adornments is given, two literals
(`specs/design-facts.md` section 6); the query's size is its body, which is
charged like any rule's and is not part of the program, so a long query over
a small program is not abandoned for its length. The default is fixed for a
program and a query, so it is the same on every run.

The factor admits every rewrite that specializes the program and refuses
one that multiplies it. A rewrite makes at most two searches, each reading
the rules of each adornment it keeps once, and one draft, copying each of
those rules once, whole or cut into pieces that hold at least its body
between them, with a magic rule per demand call and a supplementary rule
per cut: a handful of adornments per predicate, each with its rules, magic
rules and cuts, costs a few copies of the program per stage. The floor
admits every rewrite of a small program outright. What is refused is a
rewrite whose adornments multiply the program, which the memory limit would
otherwise be the only thing to stop.

**Outgrowing the limit abandons the rewrite**, whether the limit is the
default or one the caller set. A warning names the limit's value and the
predicate being adorned -- the one that exploded, which is what the reader
has to change -- and the query is then answered from the whole program's
fixed point, exactly as `-X no-magic` answers it:

```text
relic: warning: the query's Magic rewrite outgrew magic=784 while adorning p/8; answering it from the whole program
```

The whole program never answers differently, though it may meet an
undefined arithmetic result, a string too long or an invalid pattern the
rewrite would not have evaluated, and fail. A query whose first search
outgrows the limit is abandoned even when the adornments a free one
subsumes would have brought its rewrite within it.

The caller's value scales the ceiling and never removes it: zero asks for
the default, and any other value, `SIZE_MAX` included, is the ceiling the
rewrite is held to in place of the default, by the same count. The limit
therefore differs from the others in what exceeding it does. A tuple or
memory limit fails the operation, since what it refuses cannot be had
another way; this limit refuses one way of answering a query that has
another, equivalent one, so it falls back. A caller who wants no rewrite at
all has `RELIC_F_NO_MAGIC`, and one who must bound what answering from the
whole program costs has `max_tuples` and `max_memory`, which bound the
fallback as they bound the rewrite. Raising the limit is for a rewrite worth
more than the default admits: ten short rules over an 8-ary predicate and a
million facts rewrite to a few thousand literals, many times the program and
trivially cheap, where the whole program's fixed point is not. Lowering it
is for a caller who would rather pay for the fixed point than for a large
rewritten program's plans.

A generated rule is attributed to the source predicate its head belongs to:
an adorned rule and a supplementary rule to the predicate adorned, and a
magic rule to the predicate whose demand it records, so a seed is attributed
to the predicate the query calls. A generated predicate is attributed the
same way, so every diagnostic about one -- a tuple or memory limit refusing
its relation, or the memory its plans take -- names the source predicate:
`limit exceeded: tuples=10 while deriving path/2`, where the relation is
`path$bf`. A predicate generated for the query itself, a supplementary
predicate cutting the query's body, has no source predicate and is
attributed to the query, as the query's answer is: `limit exceeded:
tuples=7 while deriving the query`. The explanation of a plan, which shows
the rewrite, names the generated predicates themselves.

The limit bounds the rewritten program, not what it derives. Adornments of
one predicate with different bound columns each derive their own relation,
and on a program whose calls bind many patterns those relations together can
hold several times the tuples of the whole program's fixed point, though never
more rules than the limit admits. What a rewrite within the limit costs to
order and to hold, and the memory limit that bounds it, is
`specs/design-planning-cost.md`: a rewrite can reach the memory limit
before it outgrows its own, and then the query fails rather than being
answered from the whole program, since the memory limit is the caller's
ceiling on the process and a rewrite that reaches it is one the caller
asked not to pay for.

---

## 9. Evaluating the rewrite

### 9.1 Stratification is a guard

The generated rules are analysed by the same dependency analysis as the
program, into a schedule of their own, as a check: a rewrite that produced
recursion through negation would be caught there rather than evaluated into
nonsense.

By construction it never does. Every negated literal in a generated rule reads
a complete predicate, which no generated rule derives, so a negative edge
always leaves the generated rules and can lie on no cycle. The rewrite checks
the first half as it writes each body: a negated literal over a predicate read
on demand would read an empty relation and succeed, so it fails the query with
an internal error instead. The check covers an aggregate literal, whose
folded predicate is read completely as a negated one is: one over a
predicate not read completely would fold an empty relation, and an aggregate edge,
like a negative one, always leaves the generated rules. Should the analysis
nevertheless find one, it reports the cycle as it would for a program, and the
query fails with an internal error.

### 9.2 State

A rewritten query **starts from the facts**. It discards everything derived
first, as a materialization with stale relations does, keeping the base
stores, and leaves behind the complete part and the generated relations; the
next evaluation of any kind discards them in turn, and the answer is emptied
when its rows have been delivered. What the context holds after a query,
beside the program's facts, is then a function of that query alone, so its
counters and its memory are the same whatever ran before it. A query whose
rewrite is abandoned over its limit leaves the whole program's fixed point
instead, as a query under `-X no-magic` does. The price is that each
rewritten query derives again what it reads completely, seeding its derived
predicates with their facts again.

A query holding aggregates owns their group rules, and runs each once
between the two phases, after the complete part has made what it reads
complete and **before any generated rule runs**, since a magic or
supplementary rule may hold a copy of the literal that folds the group;
the group relations are emptied with the answer, `specs/design-aggregates.md`
section 10.

The discard runs before either phase begins, so no delta range spans it --
the obligation `specs/design-relations.md` section 4 places on any operation
that ends a span of the append-only store.

### 9.3 Why the answers are right

**Sound.** Unfolding every supplementary literal into the body of its rule
gives the rules the plain construction generates, because a supplementary
predicate carries every variable of its prefix that the rest of the rule
reads; so the argument is made for those. Every generated rule is then a
source rule with literals renamed and, for an adorned rule, a guard added;
or a magic rule, whose head no answer reads except through a guard. By induction on derivations, every tuple of
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
literal's tuple, and by the hypothesis the adorned literal holds too; each
supplementary rule on the way fires for the same reason, so the prefix it
stands for holds through it. Every
literal of the adorned rule then holds, and it derives `p$α(t)`. Applied to the
query, whose rewrite has no guard, this gives every answer.

The equivalence `magic(Q, P) == Q(full_materialize(P))` is what the test suite
checks, over the language corpus and under every strategy. Equal answers do
not show that the rewrite restricted anything, so the suite also holds a bound
query's evaluation, through the `-X stats` counters, to a fraction of what the
whole fixed point holds: a rewrite that is correct but ineffective fails.

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
magic rewrite of $q0(X) :- path(a, X).
    adorn path/2 bf
    seed magic$path$bf(a).
    stratum 0, component 0 (recursive): magic$path$bf/1
        facts: 1
        rule magic$path$bf(Y) :- magic$path$bf(X), edge(X, Y).
            delta magic$path$bf(X) when the delta holds more than 3 tuples:
                scan edge(X, Y)
                member delta magic$path$bf(X)
                emit magic$path$bf(Y)
            delta magic$path$bf(X) otherwise:
                scan delta magic$path$bf(X)
                lookup edge(X, Y) key={0}
                emit magic$path$bf(Y)
    stratum 0, component 1 (recursive): path$bf/2
        rule path$bf(X, Y) :- magic$path$bf(X), edge(X, Y).
            scan edge(X, Y)
            member magic$path$bf(X)
            emit path$bf(X, Y)
        rule path$bf(X, Z) :- magic$path$bf(X), edge(X, Y), path$bf(Y, Z).
            delta path$bf(Y, Z) when the delta holds more than 3 tuples:
                scan edge(X, Y)
                member magic$path$bf(X)
                lookup delta path$bf(Y, Z) key={0}
                emit path$bf(X, Z)
            delta path$bf(Y, Z) otherwise:
                scan delta path$bf(Y, Z)
                lookup edge(X, Y) key={1}
                member magic$path$bf(X)
                emit path$bf(X, Z)
query $q0(X) :- path$bf(a, X).
    lookup path$bf(a, X) key={0}
    stream $q0(X)
```

* The phase 1 components keep their positions in the program's schedule, so
  a component the query does not read leaves a gap in the numbering, and the
  gap is what shows that it was not evaluated. `edge` holds facts alone, so
  it is in no component and needs no evaluation: phase 1 shows nothing, and
  the plans read `edge` from its base store.
* `adorn` names the source predicate and the adornment; the adorned and magic
  names follow from them by section 4.
* A supplementary predicate is not listed under `adorn`: it belongs to the
  rule that generated it, and appears as the head of its one rule among the
  rewritten program's components -- in the component of the rules that read
  it when they are recursive through it, and in a component of its own
  otherwise.
* `seed` shows each rule the query's body generated, with a body or not. A
  component's `facts: N` counts its body-less seeds and the facts its
  members' base stores hold, as everywhere, so a body-less seed would
  otherwise be visible only as a count.
* The rule that reads a derived predicate's facts is shown as its
  adornment's first rule, and its plans read the base view by name,
  `lookup link$base(X1, X2) key={0}`: the base store of `link`, which
  `link` names the derived relation of.
* A query whose body reads only complete predicates has nothing to adorn and
  shows no rewrite heading: its phase 1 components and its plan.
* An adornment the free one subsumes is listed where it was reached, with
  the subsumption noted, so a reader sees what the query would have asked
  for:

  ```text
  adorn pointsTo/2 ff
  adorn pointsTo/2 bf, subsumed by pointsTo/2 ff
  ```
* A rewrite abandoned over its limit shows no rewrite heading. Its phase 1
  components come first, then a line marking the abandonment, then the
  whole program's components and the query's plan, as full materialization
  shows them. A component phase 1 evaluated appears on both sides of the
  line, since answering from the whole program evaluates it again; the line
  says why, and is shown even when that evaluation then fails:

  ```text
  abandoned magic rewrite of $q0(Y2, Y3) :- p(c, Y2, Y3), not h(d).
  ```

* A rewrite whose adornments bind nothing ends its heading with
  `read completely:` and the predicates the search reached, and is followed
  by the plans of the program's components it adds to phase 1 and of the
  query's own rule:

  ```text
  magic rewrite of $q0(X, Y) :- pointsTo(X, Y).
      adorn pointsTo/2 ff
      adorn pointsTo/2 bf, subsumed by pointsTo/2 ff
      read completely: pointsTo/2
  stratum 0, component 0 (recursive): pointsTo/2
  ```
* A recursive rule of the rewritten program shows its delta variants and no
  plan of its own, as `specs/design-planner.md` section 8.2 describes.
  `edge` is small and complete, so each variant's placed plan scans it
  before the delta and reaches the delta through a lookup or a membership
  test; the delta-first plan after it runs on the rounds whose delta holds
  no more tuples than the threshold.

The lines follow the rules of `specs/design-planner.md` section 8.3: printable
ASCII, escaped constants, cut at 1023 bytes.

---

## 11. Memory

The rewrite lives for one query run. Generated rules are held in a growable
rule table, with their argument and body arrays and the scratch space for
building names in an arena of the rewrite's own; the memo and duplicate
tables are identifier tables over the rewrite's arrays. What walking or
drafting one rule needs, the planner's working state and the drafts
themselves, is built in a scratch arena emptied once the rule is walked or
drafted. The walks the search records, a step per body literal of each rule
it reads, are kept in an arena of their own, emptied when the rewrite
returns, so the query runs without them. A rule found to be a duplicate or
to derive nothing is released from the rewrite's arena as soon as it is
recognized, so that arena holds the rules kept and the names generated,
however many were drafted. All of it is released
when the query has run, successfully or not, as is the schedule of the
rewritten program. Predicate entries for generated names, and the names
themselves in the predicate table's name store, belong to the context, as
every interned entry does (`specs/design-vocabulary.md` section 3).

The rewrite's arena is charged to `max_memory`, per
`specs/design-planning-cost.md` section 6: the limit of section 8 bounds
the rewrite's size in literals to a multiple of the program's, and the
memory limit bounds what that multiple costs. The growable tables beside the arena are linear in
the rules the limit admits and are not charged.

---

## 12. Determinism

* The complete part is found by a walk whose order depends only on the
  program and the query, and evaluated in schedule order.
* Adornments are recorded in the order they are first asked for, and
  processed in that order; an adornment's rule over its base view is
  generated first, then the rules of the program in program order and,
  within a rule, its magic and supplementary rules in body order and then
  the adorned rule.
* The order within a body is the planner's, which is deterministic per
  `specs/design-planner.md` section 9.
* Names derive from the predicate and the adornment, and for a
  supplementary predicate from the positions of its rule and its call.
* Duplicates are found by comparison, not by address.

So the same program and query give the same generated rules in the same order,
the same schedule, and byte-identical `-e` output.

---

## 13. What is deliberately absent

* **Demand through negation.** Architecture section 37: a negated relation is
  materialized in full. Propagating bindings into it would need the
  refinements of magic sets for stratified negation, which are correct only
  under conditions that are not obvious on first reading.
* **Demand through an aggregate.** The predicate an aggregate folds is
  read completely, as a negated predicate is, and is never adorned;
  adorning it on its
  shared columns is left with demand through negation,
  `specs/design-aggregates.md` section 20.
* **Adorning a predicate no rule derives.** Section 3.
* **A guarded copy of each fact.** Section 6.
* **A supplementary predicate before the first call binding a column.** The
  classic construction cuts there too, which would materialize a relation
  for every adorned rule, `path :- edge, path` included, to save repeating a
  prefix that is usually one literal.
* **A threshold on cutting.** Section 6.
* **A limit on the predicates or the rules generated.** Section 8: both
  are outputs of the work the limit there counts, bounded by it, and a
  bound on an output bounds the work only through a list of the ways work
  yields output. Nobody reasons about a query in adorned predicates, so a
  cap on their number is a knob without a user.
* **Failing a query whose rewrite outgrows its limit.** Section 8: the
  whole program answers the same, `RELIC_F_NO_MAGIC` serves a caller who
  wants no rewrite, and `max_tuples` and `max_memory` bound the fallback.
* **Naming supplementary predicates by content.** Section 7.
* **Relations kept across queries.** Section 9.2.
* **A rewrite of a program already materialized.** Section 2.
