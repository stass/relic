# Magic Sets

A query that binds some of its arguments needs only the part of the
program's fixed point it can reach. The Magic Sets rewrite specializes the
program to the query: each predicate the query reaches on demand is replaced
by adorned copies, one per pattern of bound arguments it is called with, and
each ordinary copy's rules are guarded by a magic relation holding the bound
values it is actually asked about. Eligible bound copies use
[query factoring](design-query-factoring.md) to propagate states with their
original binding and produce answers from base rules. The output is ordinary
compiled rules, which the ordinary analysis, planner and evaluator run. This document covers which
predicates a query reads completely, the generated rules and their names,
the order bindings flow in, the subsumption of bound adornments by a free
one, the single limit on the rewrite's work and its fallback, and why the
answers equal those of whole-program evaluation. The rewrite is an
extension of the pipeline, between query compilation and evaluation, in the
query-directed mode.

Related: [architecture: Magic Sets](architecture.md#magic-sets),
[query modes](architecture.md#query-modes),
[limits](architecture.md#limits),
[negation](language.md#negation), [aggregates](language.md#aggregates),
[generated names](architecture.md#naming), [planning](design-planning.md),
[stratification](design-stratify.md), [evaluation](design-eval.md),
[facts](design-facts.md), [aggregates](design-aggregates.md),
[bound query factoring](design-query-factoring.md).

## Model

A query is answered in one of two ways
([query modes](architecture.md#query-modes)). It is **not rewritten** when
the context's relations already hold the program's fixed point --
`relic_materialize()` ran, or `-a` was given, and the program has gained no
rule and no fact since -- because everything the query could read is then
complete and running its plan directly is cheaper than any rewrite. Nor is
it rewritten under `-X no-magic` (`RELIC_F_NO_MAGIC`), which materializes the
program and runs the plan directly: that is the reference the rewrite is
tested against.

Otherwise the query is answered in two phases, over derived relations
discarded for it:

```text
query -> find what is read completely -> materialize the complete part
      -> rewrite -> stratify -> evaluate -> run the rewritten query
```

1. **The complete part** is materialized: the program components the query
   reads in full, evaluated exactly as whole-program evaluation evaluates them,
   in schedule order.
2. **The demanded part** is rewritten: every other predicate the query
   reaches is replaced by ordinary or factored adorned copies, planned
   against the relations phase 1 made complete, and evaluated by the same
   engine. The rewritten query then runs once over the result.

There is no second execution path. The rewrite emits `relic_rule_t` values;
the dependency analysis of [design-stratify.md](design-stratify.md)
schedules them, the planner plans them, and the fixed points of
[design-eval.md](design-eval.md) run them.

The rewrite itself works in two stages. A **search** finds the set of
adornments the query reaches and drafts nothing; a **draft** then generates
the rules of the adornments the search kept. Both stages charge everything
they read or generate to one limit, and a rewrite that outgrows it is
abandoned and the query answered from the whole program.

## Invariants

* The rewrite never decides whether a query is answered, nor its answer:
  it equals whole-program evaluation's, and an abandoned rewrite falls back
  to it.
* Negated and aggregate literals in generated rules read only predicates
  read completely.
* Generated names contain `$`, so they collide with nothing a program
  writes ([naming](architecture.md#naming)).
* Ordinary demand calls follow the planner's body order; their draft replays
  the search's walks. Factored adornments replay the search's complete
  certificate instead. Neither path rediscovers its meaning while drafting.
* Everything the rewrite reads or generates is charged before it is done,
  and its remaining work is a constant multiple of that charge.
* A rewritten query changes neither the program nor what a later query
  answers, and the derived relations it leaves are a function of that query
  alone; the values and predicates it interns persist.

## What a query reads completely

A predicate the query reaches is read either **completely**, from a relation
holding every tuple the program derives for it, or **on demand**, through an
adorned copy that holds only the tuples some caller asked about.

A predicate is read completely when any of these holds:

* **No rule derives it.** Its base store, filled when its facts were
  loaded, is the whole relation, and reading it needs no evaluation
  ([design-facts.md](design-facts.md)). A predicate of facts alone is one,
  and so is a base predicate `p$base`.
* **The query reaches it through negation.** A negated relation is
  materialized in full in its lower stratum, so that absence means absence.
* **The query reaches it through an aggregate.** The predicate an
  aggregate literal folds, and every predicate a literal of the aggregate's
  body names, since a fold needs the whole group
  ([aggregates](design-aggregates.md#magic-sets)).
* **Something read completely depends on it**, positively or negatively. A
  relation is complete only if everything it is derived from is.
* **The search keeps only free adornments.** When no adornment the search
  keeps binds a column, every predicate it adorned is read completely and
  nothing is drafted ([subsumption](#free-adornments-subsume-bound-ones)).

Everything else the query reaches through positive literals is read on
demand.

**Why demand stops at negation and aggregates.** Restricting a negated or
folded predicate to the keys some caller asks about would not by itself give
wrong answers: by the [completeness argument](#soundness-and-completeness), an
adornment holds every tuple of its predicate whose bound part is demanded, so
absence, or a group, under a demanded key would be read whole. What breaks is
the schedule. The magic relation guarding the negated or folded predicate is
derived from the prefix of the rule that reads it, and that prefix can depend
on the reader's own component, which puts the strict edge on a cycle and
leaves the rewritten program unstratified. Magic sets for stratified negation
avoid that only under conditions that are not obvious and depend on the
program. Reading such predicates completely keeps every strict edge leaving
the generated rules ([stratification is a guard](#stratification-is-a-guard)),
so the rewritten program is stratified by construction, at the price of
evaluating those predicates whole.

The first four are found by one walk over the program's schedule, before
anything is evaluated:

```text
demanded = {}, roots = {}
visit the query's body:
	a positive literal over a predicate no rule derives: add it to roots
	a positive literal over any other predicate p: if p is not in
	    demanded, add it and visit the body of every rule deriving p
	a negated literal over q: add q to roots
	an aggregate literal: add the predicate it folds, and every predicate
	    a literal of its body names, to roots
complete = roots and everything they depend on
read on demand = demanded minus complete
```

The walk also visits the rules of a predicate that turns out complete. That
is harmless: whatever it finds beyond such a predicate is something the
predicate depends on, and so is complete too. The walk and the closure run
over components of the schedule rather than over predicates: every
dependency inside a component is positive, so reading one member on demand
reaches every other, and the complete part is a set of whole components.
Phase 1 is the program's schedule filtered to them.

Two consequences follow. A predicate negated somewhere and called
positively elsewhere is read completely everywhere, since it is
materialized anyway. And a component the query cannot see is neither
seeded with its facts nor run.

The last condition is known only once the adornments are, so it is decided
after the search. Its predicates are closed over components the same way,
the components they add are evaluated from the program's schedule after
the walk's, and the query rule is the one written, planned over the
program's own relations as a query against a materialized program is. This
generalizes the case where the relations already hold the fixed point: a
rewrite whose adornments bind nothing would derive exactly what evaluating
the reached components whole derives, so the program's own
relations are read instead and nothing is copied.

**Base predicates are interned before the walk.** The rewrite interns the
base predicate of every derived predicate whose base store holds facts
before it takes its snapshot of the predicate table, so every base predicate
is inside the snapshot and complete by the first rule above. The rewrite
asks whether a predicate is complete only of program predicates and base
predicates, all of
which the snapshot covers; a predicate outside it is an internal error,
reported rather than read past an array.

## Adornments and generated names

An **adornment** says, for each argument of a literal at the point the
literal is evaluated, whether its value is already known: `b` for bound --
a constant, or a variable bound earlier -- and `f` for free. A wildcard is
always free. Internally it is a column mask, bit *c* set when column *c* is
bound, which is the shape an index key already has.

Each predicate read on demand is adorned once per adornment it is called
with, and each adornment is a separate predicate. Names are built from the
predicate and the adornment, never from a counter, contain `$`, so they
collide with nothing a program writes ([naming](architecture.md#naming)),
and stay readable in explain output:

| Generated predicate | Example | Arity |
| --- | --- | --- |
| adorned copy of `path/2`, first argument bound | `path$bf` | 2 |
| its magic predicate: the bound values asked about | `magic$path$bf` | 1 |
| factored states with their original binding | `walk$path$bf` | Twice the bound-column count |
| supplementary predicate: the prefix before the second call binding a column in rule 0 deriving `r$bf` (see the example below) | `sup$r$bf$0$2` | its live variables |
| base predicate of a derived predicate `link` with facts | `link$base` | 2 |

A predicate of arity zero is adorned as `p$`. A supplementary predicate is
named `sup$`, the adorned predicate's name, the position of its rule among
the rules deriving that predicate, counted from 0 in program order, and the
position of the call it precedes among the rule's calls binding a column,
counted from 1 in body order. Cuts of the query's rule are named after
`$q0`, e.g. `sup$$q0$0$2`, whatever the query's `$qN`, since the query's
head names no predicate.

**An adornment with no bound column has no magic predicate.** A magic
relation records which bound values were asked about; with nothing bound
its only possible tuple is the empty one, and it would exist exactly when
the adorned predicate is reached at all, which the rewrite already knows.
`path$ff` is therefore a copy of `path` with no guard. It is a copy rather
than `path` itself because its rules are rewritten too: a bound call inside
one of them propagates demand like any other.

Generated predicates enter the context's predicate table like any other and
are never removed; a later query with the same adornments reuses them, and
a base predicate is interned the first time a rewrite needs it. A cut of the
query's rule named after `$q0` is reused when a later query's cut at that
position keeps as many variables, and is otherwise a predicate of the same
name and another arity. No atom of a program or query names a generated
predicate, so the undefined-predicate warning never names one.

## Sideways information passing

For each rule of an ordinary adornment, the rewrite needs the order
in which its body's literals bind their variables: a literal's adornment is
what the literals before it have bound. That order is the planner's. The
planner's ordering is split from the plan it emits (`relic_plan_order()`,
see lib/internal.h): it gives the body indexes in the order the plan would
place them -- positive literals greedily, tests at the earliest point their
slots are bound, an assignment or aggregate reporting the slot it binds --
starting from a given set of bound slots. Planning a rule for evaluation is
the same ordering from nothing bound, followed by emitting one operation
per literal. One function decides the order for both uses, and nothing else
does.

For the rewrite, the initial bound set is the variables of the head's bound
columns, and the planner is asked for an order alone, building no index and
emitting nothing
([static planning](design-planning.md#static-planning-for-magic-sets)).
The relations it can measure
are the complete ones, which phase 1 has just filled, so their tuple counts
and distinct values are real; those read on demand are marked incomplete and
estimated. This is why there are two phases: planning the rewrite before the
complete part is loaded would measure every relation as empty, and an empty
relation estimates zero and is placed first.

A bad order costs demand precision, never correctness: every order gives
the same answers, as the [soundness and completeness
argument](#soundness-and-completeness) shows. Under `-X no-reorder` the
order is the written one; combined with `-X no-factor`, this makes the
textbook left-to-right rewrite available as a reference.

## The generated rules

For an ordinary adorned copy, take a predicate *p* adorned with *α* and a
rule deriving *p*, its body in the order above. A **demand call** is a
positive literal over a predicate read on demand whose adornment *β* binds
at least one column. The draft
walks the body keeping a **prefix**: the literals evaluated so far, as the
generated rules write them. The prefix starts as the guard
`magic$p$α(...)` over the head's bound arguments, or empty when *α* binds
no column. Each positive literal over a predicate read on demand is
appended renamed to its adorned predicate; literals over complete
predicates, negated literals, comparisons and aggregate literals are
appended as they are. The draft generates:

* **one magic rule per demand call**, except a call that restates the head:
  its head is `magic$q$β(...)` over the call's bound arguments and its body
  is the prefix. It says: whenever the rule is called with these bound
  values and the literals before this one hold, this literal is called with
  these;
* **one supplementary rule before every call binding a column but the
  first** and those restating the head. The prefix is not written out again
  but derived once into a **supplementary predicate** whose columns are the
  prefix's *live* variables -- those it binds that occur in the call, in a
  literal after it, or in the head. From there the prefix is that one
  literal: the call's magic rule, every later rule and the adorned rule read
  it through the supplementary predicate;
* **the adorned rule**: the source rule with *p* renamed `p$α` and the final
  prefix as its body.

**Why cut the prefix.** Written out in full before every call, the prefixes
of a body of *k* demand calls hold on the order of *k*^2 literals, and when
recursive their plans hold on the order of *k*^3 operations. Cutting makes
the generated bodies hold about *n* + 2*k* literals over *n* body literals,
and evaluates the prefix through a call once rather than once per later call
and again for the adorned rule. A rule with one call binding a column, which
most rules have, generates exactly the classic construction: no
supplementary predicate exists. The cost is one relation per cut, holding
the prefix's live bindings, and the trade is made uniformly: a threshold on
where to cut would be a knob whose right value depends on the data, and what
a supplementary relation holds is counted by the tuple and memory limits as
every relation is. A cut before the *first* call binding a column is not
made: it would materialize a relation for every adorned rule, `path :- edge,
path` included, to save repeating a prefix that is usually one literal.

Why a cut loses nothing: it keeps the slots bound before the call that are
read at or after it. A slot it drops is bound and absent from the prefix
from then on, but nothing from the call onward reads it.

For `r(X, W) :- a(X, Y), b(Y, Z), c(Z, W).` with `r(1, W)?` and `a`, `b`,
`c` derived, the draft generates (seed and the rules of `a`, `b`, `c`
omitted):

```prolog
magic$a$bf(X)         :- magic$r$bf(X).
sup$r$bf$0$2(X, Y)    :- magic$r$bf(X), a$bf(X, Y).
magic$b$bf(Y)         :- sup$r$bf$0$2(X, Y).
sup$r$bf$0$3(X, Z)    :- sup$r$bf$0$2(X, Y), b$bf(Y, Z).
magic$c$bf(Z)         :- sup$r$bf$0$3(X, Z).
r$bf(X, W)            :- sup$r$bf$0$3(X, Z), c$bf(Z, W).
```

`Y` is not live past `b`'s call, so the second cut drops it.

**Wide cuts.** A supplementary predicate is a predicate, of at most 32
columns, while a rule may have up to 64 variables
([limits](language.md#limits)). A cut whose live variables do not fit is not
made: the prefix continues through that call, written out in full in the
call's magic rule, to the next call where the cut fits or to the adorned
rule. Such a body is bounded as the classic construction's is, by the
rewrite's limit, which charges every prefix the draft copies.

**Cuts follow the columns calls bind, not the adornments they read.** A
call the free adornment subsumes binds its columns like any other and has
the prefix cut before it, though it reads the free adornment and has no
magic rule. So every rule drafted for an adornment is as long as it would be
without subsumption. Were the cut before a subsumed call skipped, the
prefix would run on into the next rule and, past a later cut too wide to
make, into every magic rule after it and into the adorned rule.

### A generated rule is a compiled rule

Generated rules are ordinary compiled rules, so nothing downstream
distinguishes them. Their variables are **renumbered** in order of first
occurrence, head then body, keeping the names of the variables they came
from. Renumbering keeps the invariant that every slot below the variable
count is bound by a positive literal, which a magic rule would otherwise
break by leaving out variables of its source rule, and it gives each rule a
canonical form, which deduplication compares.

Every generated rule is safe by construction. A magic rule's head reads only
columns its adornment calls bound, which the guard or a literal before it
binds; a supplementary rule's head reads only variables its prefix binds; a
test is placed only where its slots are bound; an adorned rule reads the
variables its source rule did, through the same positive literals.

A comparison is copied, renumbered, hashed and compared by walking its
expression trees, a function call node like an operator node. A copied
`match` keeps its pointer to the pattern compiled with its source rule,
which lives in the program arena that outlives every rewrite
([design-expressions.md](design-expressions.md)).

**An aggregate literal is appended as it is.** The planner's order places it
where its shared slots are bound and reports that it binds its result slot,
so a call after it whose argument is the result is adorned bound. The copy
in a prefix has its shared and result slots renumbered and keeps the
predicate it folds, its key columns and its compiled body; the folded
predicate is read completely and never adorned.

## Factored bound adornments

An eligible bound adornment uses
[bound query factoring](design-query-factoring.md) in place of its ordinary
recursive copies. Its source rules prove a separation between state and
unchanged outputs over complete positive inputs. The search records that
certificate for the whole adornment and discovers no calls from the
recursive atom it replaces. The draft emits a walk seeded from the magic
relation, transitions that retain the original binding, and answers from
the base rules and source facts. Ordinary callers still request and read
the same adorned predicate.

Factoring does not promote another predicate into the complete set. A
failed certificate uses ordinary walks for every rule of the adornment.
Free-call subsumption takes precedence over either mode, before predicates
are named or rules drafted. `-X no-factor` selects ordinary Magic while
leaving the other query optimizations enabled.

## Facts and the base predicate

The facts a program states are tuples of their predicate's base store, not
rules ([design-facts.md](design-facts.md)). The rewrite names the base store
of a derived predicate *p* as a predicate of its own, the **base predicate**
`p$base`, of *p*'s arity, which no rule derives and which reads *p*'s base
store. How the store side keeps it is [facts](design-facts.md)'s. Each
adornment of a derived predicate whose base store holds facts is given, before
the adornments of the program's rules deriving *p*, the adornment of the rule
`p(X1, ..., Xn) :- p$base(X1, ..., Xn).`:

```prolog
link$bf(X1, X2) :- magic$link$bf(X1), link$base(X1, X2).
```

rather than a guarded copy of each fact. The base predicate is complete, so
the
planner looks the facts up by the bound columns like any complete relation;
the adornment reads only the facts its demand reaches; and the rewrite's
size does not depend on how many facts there are. The rule calls nothing on
demand, so it generates no magic rule, cuts no prefix and names no
supplementary predicate. Being first, it makes an ordinary adornment's rows
begin with its facts, as a derived predicate's rows do when materialized.
A factored adornment uses the same virtual base rule to form an answer
from each reached state; its walk seed precedes that answer rule. The
source facts are still read through one base view, never copied per fact.

## The query and its seeds

The query is a rule too, whose head is its output columns and names no
predicate ([design-planning.md](design-planning.md)). It is rewritten with
nothing bound and no guard; the rewritten rule keeps that head and runs into
the query's own answer relation. Every rule the query's body generates -- its
magic rules and the supplementary rules cutting it -- is a **seed**. With no
guard and nothing before the first literal, the first seed of `path(a, X)?` is
the body-less rule `magic$path$bf(a).`, found by the one construction rather
than built separately; `path(a, X), path(X, Y)?` also has the seed
`sup$$q0$0$2(X) :- path$bf(a, X).` A body-less seed is the only body-less rule
a rule table holds, since a program's facts are in base stores, and every
evaluation of the rewrite inserts it when its component's turn comes. The
rewritten query, `$q0(X) :- path$bf(a, X).`, is kept apart from the generated
rules and run once after them, as a query always is.

For the graph program

```prolog
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
path(a, X)?
```

the ordinary rewrite, selected by `-X no-factor`, generates

```prolog
magic$path$bf(a).
path$bf(X, Y) :- magic$path$bf(X), edge(X, Y).
path$bf(X, Z) :- magic$path$bf(X), edge(X, Y), path$bf(Y, Z).
magic$path$bf(Y) :- magic$path$bf(X), edge(X, Y).
```

The magic relation is the set of nodes reachable from `a`, and `path$bf`
holds paths starting at those nodes only. `edge` holds facts alone, so it is
read from its base store with no evaluation at all.

The default factored construction keeps the original binding in
`walk$path$bf` and derives answers only for that binding; its generated
rules and positive-length argument are in
[the factoring example](design-query-factoring.md#a-single-source-closure).

## Free adornments subsume bound ones

**Rule.** Within one rewrite, once the query reaches a predicate `p` with
the adornment that binds no column, every call to `p` in every generated
rule, whatever columns it binds, is a call to `p$ff`, and no other adornment
of `p` is generated.

**Why it is correct.** `p$ff` holds every tuple of `p` in the program's
least model, by completeness for an adornment that binds nothing, and only
such tuples, by soundness. A call adorned `bf` reads the tuples of `p`
whose first column is the bound value, which are the tuples of `p$ff` whose
first column is that value: a lookup into `p$ff` on that column, which the
planner gives it as it gives any bound literal. Nothing the bound adornment
could hold is missing, and nothing in it was ever cheaper than what the free
copy holds, since the free copy is computed in full beside it. Without the
rule, a free query over a points-to analysis builds a bound copy and a magic
relation per call pattern beside the free copy, holding several times the
derivations of the program run whole.

The redirected call binds the same variables the adorned call bound, so the
literals after it see the same bound set and their adornments do not
change. Its magic rule is not generated, since a call to `p$ff` propagates
no demand. Stratification remains a guard: redirection adds positive edges
to `p$ff` and removes edges to `p$bf` and its magic predicate, and a negated
literal names a predicate read completely, so no cycle through negation can
appear.

**Adorn first, draft second.** A rewrite that drafted rules as it
discovered adornments could reach a bound adornment of `p` before the free
one and would have to withdraw what it drafted. So the search finds the
adornments before anything is drafted. For an ordinary adornment it walks
each rule's body once for the adornment of its head: it orders the body with the planner, records for
each literal the variables bound before it, whether it calls a predicate
read on demand and which columns such a call binds, reaches the adornment
each call reads, and drafts nothing. The kept adornments are the worklist:
reading one adornment's rules keeps the adornments its calls need. A
predicate is read free from the first call that reaches it free, and every
later call on it, whatever it binds, reaches the free adornment.

A factored adornment's certificate replaces those walks and has no outgoing
demanded calls. A later free call replaces that bound candidate with the
ordinary free adornment; no walk predicate is drafted for the subsumed
candidate. Certification and ordinary call discovery are both fixed for
one source, adornment and complete set, so the two-search argument also
applies to this mode.

**Why two searches, and why not three.** An adornment of `p` binding
columns that a call reached *before* `p` was first reached free was reached
for nothing, and so was whatever only it reached. When the first search kept
one, the search runs once more, with every predicate the first read free
read free from the start. No third search is needed: which adornments a
rule's calls reach depends only on the adornment the rule is read for and on
which predicates are read free. By induction over what the second search
reaches, every call it makes the first made too -- a call reaching a free
adornment reaches one the first reached, and a call binding columns of a
predicate the first never read free reaches the adornment the first reached
for it -- so every predicate the second search reaches free is already read
free, and no adornment it keeps was reached for nothing. The free marks are
kept whole: a predicate the first search read free stays free in the second
even when only an adornment the second does not reach called it free, and
is then computed in full beside the bound calls it serves.

The second search reuses the ordinary walks or factoring certificate the
first recorded. Within one rewrite a walk depends on the rule and on the
adornment it is read for -- which predicates are read on demand, and the
measures of those read completely, do not change while it runs -- and not on
which predicates are read free, which decides only the adornment a call
reaches. The draft replays the walks the last search kept, with the
adornment each call reached, or emits the retained certificate's rules,
so search and draft cannot disagree.

When no kept adornment binds a column, nothing is drafted and the query
reads what it reached completely ([what a query reads
completely](#what-a-query-reads-completely)).

## Termination and structural deduplication

**The worklist is memoized on the predicate and the bound-column mask.**
Reaching `(p, α)` for the first time records it and queues the rules of
*p*; reaching it again finds the record. Each record is processed once, so
the rewrite ends when every recorded adornment is processed. A predicate of
arity *n* has at most 2^*n* adornments -- finite, since arity is at most 32,
but not small -- so the practical bound is the rewrite's limit, which
charges the rules read for each adornment kept. Records are processed in
the order they were first reached, which is also the order explain lists
them in.

**Duplicate rules are dropped.** Two source rules sharing a prefix generate
the same magic rule up to the names of their variables:

```prolog
p(X) :- a(X), q(X).
p(Y) :- a(Y), q(Y), s(Y).
```

both give `magic$q$b(X) :- magic$p$b(X), a(X).` after renumbering. Each
generated rule is compared structurally with those already kept, through a
hash table over their canonical forms, and a duplicate is not appended.
Renumbering is what makes a plain structural comparison enough: two rules
equal up to renaming have equal canonical forms. An aggregate literal enters
the canonical form by its function, the predicate it folds, its key columns,
its aggregated column and its slots. Supplementary predicates are named by
their rule and call, so two source rules sharing a prefix past their first
call binding a column generate distinct supplementary rules, and at most the
magic rules before their first cut are found equal. Sharing further would
mean naming a supplementary predicate by its content, and what it would
save is linear in what is kept.

**A rule whose head is one of its own positive literals is dropped.** It
can only derive a tuple the relation already holds. It arises naturally:
`p(X, Y) :- p(X, Z), e(Z, Y).` adorned `bf` generates
`magic$p$bf(X) :- magic$p$bf(X).`, which would otherwise make a recursive
component out of nothing.

**A call that restates the head has no magic rule and no cut before it.**
A call on the head's predicate that binds the head's bound columns, and no
others, to the head's own arguments there reads the adornment its rule is
rewritten for with the very tuple the guard holds. Its magic rule would
derive only that tuple. Over the uncut prefix, which starts with the guard,
it is the self-literal rule above and is dropped; but behind a cut it reads
the supplementary predicate rather than the guard, the restatement is
hidden, and the magic and supplementary relations would be derived from each
other to no end. In

```prolog
p(X, Y) :- q(X), p(X, Z), s(Z, Y).
```

as rule 0 deriving *p*, adorned `bf`, with *q* derived, the call on *p* is
the second call binding a column, and its magic rule would be
`magic$p$bf(X) :- sup$p$bf$0$2(X).` So a restating call records nothing and
no cut is made before it: the cut would serve only the rules after the call,
which read the prefix written out instead, up to the next call binding a
column, where it is cut as it would have been. The call counts among the
calls binding a column, so the supplementary predicates after it keep their
names. The common case is a recursive rule queried on the column its
recursive call shares with the head: for `path(X, Z) :- edge(X, Y), path(Y,
Z).` and `path(X, d)?`, the planner places `path$fb(Y, Z)` first, binding
only `Z`, the head's bound argument, so `path$fb` needs no demand beyond the
seed.

## Stratification is a guard

The generated rules are analysed by the same dependency analysis as the
program, into a schedule of their own. The analysis is needed to order the
generated predicates into components and find the recursive ones; as a check
it is redundant by construction. Every negated literal and every aggregate
literal in a generated rule reads a complete predicate, which no generated
rule derives, so a negative or aggregate edge always leaves the generated
rules and can lie on no cycle.

The rewrite checks the first half as it writes each body: a negated or
aggregate literal over a predicate read on demand would read an empty
relation -- a negation would succeed, a fold would see no group -- so it
fails the query with an internal error instead of evaluating nonsense.
Should the analysis nevertheless find a cycle through negation, the query
fails with an internal error rather than a program diagnostic, since the
program did nothing wrong.

## The work limit

### One count, charged before the work

Finite is not small: a program whose rules each call a predicate with one
more column bound than the head was called with reaches all 2^*n*
adornments, and a few kilobytes of such a program rewrite into thousands of
adornments and gigabytes of rules where whole-program evaluation takes
megabytes. The rewrite is therefore bounded by one limit per query,
`max_magic` (`-l magic=N`), in one unit: the **size of a literal**, one,
and one more per node of its expressions, so a literal carrying a large
expression costs what walking and copying its nodes costs. A rule's size is
its body's plus one for the guard an adorned copy is given. An aggregate
literal is one literal: what it folds is read completely and its body is
never copied. Facts count nothing: the rewrite copies none, so a thousand
facts behind one rule rewrite as cheaply as one.

**Everything the rewrite reads or generates is charged where it is read or
generated, before the work.**

* A search charges each rule it reads, its body's size, before the rule is
  walked or its walk replayed. Every search charges what it reads, so a
  second search is charged again for the walks of the first it replays.
  Factoring recognition and certificate replay use the same charge. A
  rejected recognition that also needs an ordinary walk charges both reads.
* The draft charges each rule it drafts before the draft is canonicalized
  and compared with the rules kept, whether the rule is then kept or dropped
  as a duplicate or as one that derives nothing. A dropped rule is built
  before it is recognized, and a long body calling its own predicate drafts
  copies that are all dropped, so a charge on what is kept would never see
  the work of building and comparing them.
* The query is charged as a rule is: its walk by each search, its rewritten
  body by the draft.
* Every generated predicate is named in one place, and naming one charges a
  literal per fixed number of bytes of its name (see lib/magic.c) before it
  is stored. A generated name
  embeds its source's, so a long name is copied, hashed and stored once per
  predicate generated from it, which no count of literals would see; the
  charge is nothing for an ordinary name.

Nothing else in the rewrite loops over anything but the program, once each:
mapping its components, indexing its rules by head, interning base
predicates,
finding what the query reads completely, and closing what it reaches over
its dependencies when nothing is bound. Ordering a body is the planner's
work, charged to the memory limit as all planning is; the charge here counts
the bodies ordered. So the rewrite's work after its first pass over the
program is a constant multiple of the literals charged, and the charge
measures the work rather than some output of it.

### One count bounds everything, by construction

* **Adornments.** Every adornment a search keeps is of a derived predicate,
  so the search reads at least one rule for it, and a rule has at least one
  body literal (a statement with none is a fact). A search keeps no more
  adornments than the literals it is charged, and the rewrite generates at
  most three predicates per adornment: adorned answers, magic requests and,
  for a factored adornment, walks. The extra walk also has a charged seed
  rule.
* **Supplementary predicates.** Each is the head of exactly one rule,
  drafted and charged with at least the prefix it cuts.
* **Rules.** Every generated rule is charged its body before it is kept, and
  every one but a seed has at least one literal: an adorned rule its guard, a
  magic rule the literal it records a call from, a supplementary rule the
  prefix it cuts, a base-predicate rule its base predicate. A seed is the
  magic rule of a call the query makes, whose walk was charged, or of an
  adorned rule's first call when constants alone bind it, at most one per
  adorned rule, whose draft was charged. So the rules kept number at most
  twice the literals charged.
* **Working memory.** A search holds its adornments and recorded walks or
  factoring certificates, bounded by the source bodies inspected. They grow
  with the literals charged, are charged to the memory limit, and are released
  once the draft is done. The planner's working state for one rule is
  released once the rule is walked or drafted.

**Why one limit on work, and none on outputs.** A limit on predicates or
rules generated counts outputs, and an output bounds work only through a
case argument -- every kept adornment yields a kept rule, every kept rule
holds a literal -- which is only ever as good as its list of exceptions.
Separate limits per stage, each lifted by some configuration of the others,
make correctness rest on every cell of a matrix of configurations bounding
every stage; such a matrix keeps turning up a cell that bounds nothing. One
count over all work has no such cell: there is no configuration in which a
stage runs unmetered, because there is nothing to lift. A caller's value
replaces the ceiling and never removes it: zero asks for the default, and
any other value, `SIZE_MAX` included, is the ceiling the rewrite is held to
by the same count. Nobody reasons about a query in adorned predicates, so a
cap on their number would be a knob without a user.

### The default is in proportion to the program

A rewrite must not generate a program unboundedly larger than the one it
specializes, so the default is a floor plus a fixed multiple of the size of
the program and the query (constants in lib/magic.c). The program's size is
what one adorned copy of it holds: each rule's body plus one for its guard,
with its head and each positive call counting a literal per that fixed number
of bytes of the predicate's name, as the names generated for them are charged;
a derived predicate whose base store holds facts counts as the two literals of
its base-predicate rule. The query's size is its body, charged like any rule's
and not part of the program, so a long query over a small program is not
abandoned for its length. The default is fixed for a program and a query, so
it is the same on every run.

The multiple admits every rewrite that specializes the program and refuses
one that multiplies it. A rewrite reads the rules of each adornment it
keeps at most four times, each reading charged:

* the first search certifies the adornment for factoring,
* and walks it as an ordinary adornment when a rule fails the certificate;
* the second search replays what the first found, certificate or walks;
* the draft copies each rule once -- whole, or cut into pieces holding at
  least its body between them -- with a magic rule per demand call and a
  supplementary rule per cut, or a walk seed, a transition per recursive
  rule and an answer rule per base rule for a factored adornment.

A handful of adornments per predicate costs a few copies of the program per
reading; the default allows each of the four readings the same floor and
factor, and is the same whether or not factoring is enabled, so that
`-X no-factor` changes how a query is rewritten and not what it may spend.
A failed certification is real work, and is charged, rather than hidden by
a default that does not count it. The floor admits every rewrite of a small
program outright.

### Outgrowing the limit falls back

A rewrite that outgrows the limit, default or set, is **abandoned**. A
warning names the limit's value and the predicate being adorned -- the one
that exploded, which is what the reader has to change -- or the query, when
its own walk or rewritten body is what outgrew it:

```text
relic: warning: the query's Magic rewrite outgrew magic=3136 while adorning p/8; answering it from the whole program
```

The query is then answered from the whole program's fixed point, exactly as
`-X no-magic` answers it. The whole program never answers differently,
though it may meet an undefined arithmetic result, a string too long or an
invalid pattern the rewrite would not have evaluated, and fail
([evaluation errors](language.md#evaluation-errors)). A query whose first
search outgrows the limit is abandoned even when subsumption would have
brought its rewrite within it.

This limit differs from the others in what exceeding it does. A tuple or
memory limit fails the operation, since what it refuses cannot be had
another way; this limit refuses one way of answering a query that has
another, equivalent one, so it falls back. A caller who wants no rewrite at
all has `RELIC_F_NO_MAGIC`; one who must bound what answering from the whole
program costs has `max_tuples` and `max_memory`, which bound the fallback as
they bound the rewrite. Raising the limit is for a rewrite worth more than
the default admits -- a few short rules over a wide predicate and millions
of facts can rewrite to many times the program and be trivially
cheap beside the fixed point. Lowering it is for a caller who would rather
pay for the fixed point than for a large rewritten program's plans.

The limit bounds the rewritten program, not what it derives. Adornments of
one predicate with different bound columns each derive their own relation,
and on a program whose calls bind many patterns those relations together can
hold several times the tuples of the whole fixed point. A rewrite can reach
the memory limit before it outgrows its own; the query then fails rather
than falling back, since the memory limit is the caller's ceiling on
accounted storage and evaluation memory, not on the process, and a rewrite
that reaches it is one the caller asked not to pay for.

### Attribution

A generated rule is attributed to the source predicate its head belongs to:
an adorned, supplementary or factored walk rule to the predicate adorned,
a magic rule to the predicate whose demand it records, so a seed is attributed to the
predicate the query calls. Generated predicates are attributed the same way,
so every diagnostic about one -- a tuple or memory limit refusing its
relation, or the memory its plans take -- names the source predicate:
`limit exceeded: tuples=10 while deriving path/2` where the relation is
`path$bf`. A supplementary predicate cutting the query's body has no source
predicate and is attributed to the query (`limit exceeded: tuples=7 while
deriving the query`), while the rule deriving it, like a seed, is attributed
to the predicate of the call it precedes. Explain output, which shows the
rewrite, names the generated predicates themselves.

## Evaluation and state

A rewritten query **starts from the facts**. It discards everything derived
first, keeping the base stores, and leaves behind the complete part and the
generated relations; the next evaluation of any kind discards them in turn,
and the answer is emptied once its rows are delivered. So a query changes
neither the program nor what a later query answers, and the derived
relations it leaves are a function of that query alone. The values,
predicates and counts it interns persist for the life of the context
([growth for life](design-values.md#growth-for-the-life-of-the-context)),
so memory headroom and counters may depend on earlier runs. The price is
that each rewritten query derives again what it reads completely. A query
whose rewrite is abandoned leaves the whole program's fixed point instead,
as `-X no-magic` does. The discard runs before either phase begins, so no
delta range spans it -- the obligation the append-only store places on any
operation that ends a span ([design-storage.md](design-storage.md)).

A query holding aggregates owns their group rules, and runs each once
between the two phases: after the complete part has made what it reads
complete and **before any generated rule runs**, since a magic or
supplementary rule may hold a copy of the literal that folds the group. The
group relations are emptied with the answer
([design-aggregates.md](design-aggregates.md)).

The rewrite's storage lives for one query run, in arenas charged to
`max_memory`: one for the rules kept and the names built, a scratch arena
emptied after each rule is inspected or drafted, and one for recorded walks
and factoring certificates, emptied when the rewrite returns. A rule found
to be a duplicate or to derive nothing is released as soon as it is recognized, so the rewrite holds the
rules kept, however many were drafted.

## Soundness and completeness

**Sound.** Unfolding every supplementary literal into the body of its rule
gives the ordinary rules the classic construction generates, because a
supplementary predicate carries every variable of its prefix that the rest
of the rule reads; so the argument is made for those. Each ordinary generated
rule is then a source rule with literals renamed and, for an adorned rule, a
guard added; or a magic rule, whose head no answer reads except through a
guard. By induction on derivations, every tuple of `p$α` is a tuple of *p*
in the program's least model: a guard only restricts, an adorned literal
holds a subset of its predicate by the induction hypothesis, and a complete
literal, negated or not, reads the relation whole-program evaluation computes,
because phase 1 computed it the same way. Subsumption replaces an adorned
literal by a lookup into `p$ff`, which holds a subset of *p* by the same
argument. So every answer is an answer of whole-program evaluation.

**Complete.** Claim: if `p(t)` is in the least model, and either *α* binds
no column or `magic$p$α` holds the bound part of *t*, then `p$α(t)` is
derived. For an ordinary adornment, induct on the height of a derivation of
`p(t)`: take the rule instance at its root and walk its body in the planner's
order. At each
literal over a predicate read on demand with adornment *β*, the magic rule
for that literal fires: its guard holds by assumption, and every literal
before it holds -- complete ones and tests because the least model agrees,
adorned ones by the induction hypothesis, their derivations being shorter
and their own demand established earlier in the walk. So `magic$q$β` holds
the bound part of that literal's tuple, and by the hypothesis the adorned
literal holds too. Each supplementary rule on the way fires for the same
reason, so the prefix it stands for holds through it. A restating call needs
no magic rule: its demand is the guard's own tuple, which holds by
assumption. A subsumed call reads `q$ff`, which holds every tuple of *q* by
the claim for an adornment binding nothing. Every literal of the adorned
rule then holds, and it derives `p$α(t)`.

For a factored adornment, the
[factoring equivalence](design-query-factoring.md#least-fixed-point-equivalence)
establishes both claims: its adorned answers are source tuples, and it
derives every source answer for each requested original binding. This
holds for growing sets of requests, so it composes with ordinary callers
in a positive fixed point. Applying the combined argument to the query,
whose rewrite has no guard, gives every answer.

That the rewrite answers every query as whole-program evaluation does is what
the test suite checks, over the language corpus and under every strategy.
Equal answers do not show that the rewrite restricted anything, so the suite
also holds a bound query's evaluation, through the `-X stats` counters, to a
fraction of what the whole fixed point holds: a rewrite that is correct but
ineffective fails ([design-testing.md](design-testing.md)).

## Explain

With `-e`, a rewritten query shows, in the order it runs: the phase 1
components, keeping their positions in the program's schedule so that a
component the query does not read shows as a gap; a `magic rewrite of`
heading listing the adornments in the order recorded (a subsumed one noted
`subsumed by p/2 ff`), the seeds, and `read completely:` when nothing is
bound; the components of the rewritten program nested beneath it; and the
rewritten query's plan. The factored graph program above has this structure
(operation details omitted):

```text
magic rewrite of $q0(X) :- path(a, X).
    adorn path/2 bf, factored
    seed magic$path$bf(a).
    stratum 0, component 0: magic$path$bf/1
        facts: 1
    stratum 0, component 1 (recursive): walk$path$bf/2
        rule walk$path$bf(Origin$0, Origin$0) :- magic$path$bf(Origin$0).
            ...
        rule walk$path$bf(Origin$0, Y) :- walk$path$bf(Origin$0, X), edge(X, Y).
            ...
    stratum 0, component 2: path$bf/2
        rule path$bf(Origin$0, Y) :- walk$path$bf(Origin$0, X), edge(X, Y).
            ...
query $q0(X) :- path$bf(a, X).
    lookup path$bf(a, X) key={0}
    stream $q0(X)
```

* A walk predicate is not listed under `adorn`; `, factored` marks the
  source adornment it implements. Its generated rules appear in normal
  component output.
* A supplementary predicate is not listed under `adorn`: it belongs to the
  rule that generated it and appears as the head of its one rule, in the
  component of the rules that read it when they are recursive through it,
  and in a component of its own otherwise.
* A base-predicate rule is its adornment's first rule, and its plan reads
  the base predicate by name: `lookup link$base(X1, X2) key={0}`.
* An abandoned rewrite prints phase 1's components first, then an
  `abandoned magic rewrite of` line, then the whole program's components
  and the query's plan. A component phase 1 evaluated may appear on both
  sides of the line, and the line is printed even when the whole program's
  evaluation then fails (tests/lang/explain-abandoned).

The line grammar is owned by [design-planning.md](design-planning.md).

## Determinism

* The complete part is found by a walk whose order depends only on the
  program and the query, and is evaluated in schedule order.
* Adornments are recorded in the order they are first reached and processed
  in that order. Within an ordinary adornment, its base-predicate rule comes
  first, then the program's rules in program order, and within a rule its magic and
  supplementary rules in body order and then the adorned rule. A factored
  adornment instead emits its walk seed first, its virtual base answer,
  then its certified source rules in program order.
* Body order is the planner's, which is deterministic.
* Names derive from the predicate and the adornment, and for a
  supplementary predicate from the positions of its rule and its call.
* Duplicates are found by structural comparison, not by address.
* The default limit is a function of the program and the query.

So the same program and query give the same generated rules in the same
order, the same schedule, the same fallback decision and byte-identical
explain output.

## Alternatives not taken

* **Demand through negation or aggregates.** See
  [what a query reads completely](#what-a-query-reads-completely).
* **A guarded copy of each fact.** It makes the rewrite's size depend on
  the data.
* **Limits on predicates or rules generated, or one limit per stage.**
  Outputs bound work only through a list of exceptions.
* **Failing a query whose rewrite outgrows its limit.** The whole program
  answers the same.
