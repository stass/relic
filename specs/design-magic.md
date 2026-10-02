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
* **the query reaches it through an aggregate**: the group predicate of an
  aggregate literal, and every predicate a literal of its group rule's body
  names, since a fold needs the whole group, `specs/design-aggregates.md`
  section 11;
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
	an aggregate literal: add its group predicate to roots, and add
	    every predicate a positive or negated literal of its group
	    rule's body names to roots
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
any rule. The search reads each rule's body in the order
`relic_plan_order()` gives it for the adornment of its head, which is the
order its draft reads, reaches the adornment of each call on a predicate
read on demand, and drafts nothing. A predicate is read free from the first
call that reaches it free, and every later call on it, whatever it binds,
reaches the free adornment. An adornment of `p` binding columns that an
earlier call reached was reached on behalf of a call the draft will not
make, and so was whatever only it reached; when the search kept one, it
runs once more, with every predicate the first search read free read free
from the start. No third search is needed. Which adornments a rule's calls
reach depends on the adornment the rule is read for and on which predicates
are read free, and on nothing else, so every call the second search makes
the first made too, and every predicate it reaches free is one the first
read free already. The adornments the last search kept are named and
drafted. When none of them binds a column, the query reads every predicate
the search adorned completely, per section 3, and nothing is drafted.

When some adornment binds a column, a free copy `p$ff` stays a copy: its
rules may call a predicate `q` with a binding that nothing else asks for,
and that demand is worth propagating. The subsumption rule has already made
sure that no such call is to a predicate that is also reached free.

Generated predicates enter the context's predicate table like any other and
are never removed, per `specs/design-program.md` section 3. Their number is
bounded by the adornments the program admits, and a second query with the
same adornments reuses them. A supplementary predicate cut from the query's
rule is named after `$q0` whatever the query, so a later query reuses it
when its cut at that position keeps as many variables, and otherwise gets a
predicate of the same name and another arity. No atom of a program or a
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

* **one magic rule per demand call**: its head is `magic$q$β(...)` over the
  call's bound arguments and its body is the prefix. The magic rule says:
  whenever the rule is called with these bound values, and the literals
  before this one hold, this literal is called with these;
* **one supplementary rule before every call binding a column but the
  first**, whether or not the call propagates demand, as a call the free
  adornment subsumes (section 4) does not: the prefix is not written out
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
rule. Such a body is bounded as the plain construction is, by the literal
budget of section 8.

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
renumbered and keeps its group predicate and group rule; the group
predicate is read completely, section 3, and is never adorned.

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
`specs/design-strings.md` section 10. A generated rule's budgets count a
comparison's nodes, so a call costs what its nodes do.

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
generated rule is compared structurally with those already generated,
through a hash table over their canonical forms, and a duplicate is not
appended. Renumbering is what makes a plain structural comparison enough:
two rules equal up to renaming have equal canonical forms. An aggregate
literal enters the canonical form by its function, its group predicate, its
aggregated column and its slots, `specs/design-aggregates.md` section 11.
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
the rewrite, names the generated predicates themselves. Supplementary predicates are not counted
by the predicate limit or its budget: each has exactly one rule, which the
rule limit and the literal budget count, so bounding rules bounds them, and
the predicate limit bounds adornments, of which the arity admits 2^n.
Counting them would abandon, against a budget of a few hundred predicates,
the rewrite of a body of hundreds of calls that the construction of section
6 exists to keep small. Exceeding a limit fails the query as any limit does: no rows, and a
nonzero exit.

**A rewrite nobody has sized has budgets.** Finite is not small: a program whose
rules each call a predicate with one more column bound than the head was
called with reaches all 2^arity adornments. Three kilobytes of arity 14 do
it, generating 16 384 adornments in 1.3 GB, where the query answered from the
whole program takes a megabyte. The rewrite is an optimization, and an
optimization must not decide whether a query is answered, nor generate a
program unboundedly larger than the one it specializes. So when neither
Magic limit is configured, the rewrite has budgets in proportion to the
program:

| Budget | Counts | Is |
| --- | --- | --- |
| generated predicates | as `max_magic_preds` | 256 + 16 x the program's derived predicates |
| generated literals | body literals of every rule drafted, kept or dropped, and of every rule a search for the adornments reads | 256 + 16 x the program's size in literals |

A program's derived predicates are the members of its schedule, and its
size in literals is what one adorned copy of it holds: each rule's body plus
one for its guard. A derived predicate whose base store holds facts counts
as the one rule over its base view that each of its adornments is given,
two literals (`specs/design-facts.md` section 6). An aggregate literal is
one literal wherever a literal is counted: its body is a group rule read
completely and never copied, `specs/design-aggregates.md` section 11.
Facts count nothing: the
rewrite copies none of them, so a thousand facts behind one rule rewrite as
cheaply as one. Both budgets are fixed for a given program, so they are the
same on every run.

**The literal budget bounds the rules kept, so generated rules have a limit
but no budget.** Every rule the rewrite generates is charged its body
literals before it is kept, and every one but a seed has at least one: an
adorned rule its guard, a magic rule the literal it records a call from, a
supplementary rule the prefix it cuts, a rule over a base view the view. A
seed is the query's, at most one per call the query makes, or the magic
rule of an adorned rule's first call when constants alone bind it, at most
one per adorned rule, whose draft was charged at least one literal. So the
rules kept number at most twice the literals drafted plus the query's
seeds. A budget on rules in
proportion to the program's rules would be the stricter only where it is
wrong: one adornment of a single rule of k demand calls keeps a magic rule
and a supplementary rule per call, about 2k rules from one rule of the
program, which is a rewrite that specializes the program and that the
budgets exist to admit.

**The search is charged what it reads.** Finding the adornments reads every
rule of each, and a query can reach far more adornments than its rewrite
could draft: the program of 2^arity adornments above does it. So each
search is held on its own to what bounds the draft: the adornments it keeps
count against the predicate limit or budget, each rule it reads against the
rule limit, and its body against the literal budget, counted apart from the
drafts. A search that outgrows one refuses or abandons the rewrite before
anything is drafted, whichever a configured limit or a budget does.
Drafting an adornment drafts every rule of its source, whole or cut into
pieces that hold at least its body between them, and the literal budget
charges every draft, so a search reads no more literals than drafting what
it keeps is charged. A search keeps no more adornments than the rewrite
without subsumption would adorn, so neither the predicate limit nor the
literal budget refuses a query that rewrite would admit. The rule limit
counts only the rules the draft keeps, which drops duplicates and rules
that derive nothing, so under a configured rule limit a search can refuse a
query whose rewrite would have fit. A query whose first search reaches more
than a limit admits is refused even when the adornments a free one subsumes
would have brought the rewrite within it. What a search needs
to read one rule, the rule's order and the planner's working state, is
released once the rule is read, so a search holds its adornments, kept and
subsumed, and nothing else that grows with the rules it reads.

The literal budget counts every rule drafted, kept or dropped. A duplicate,
or a rule that derives nothing, is built before it is recognized as one,
and a long body calling its own predicate drafts copies that are all
dropped: counting only what is kept would never see them, and their cost
in the arena would grow as the square of the body until the memory limit,
if any, refused it. The literal budget has no configurable limit behind
it: it is not a knob but the invariant that a rewrite nobody has sized is
a constant multiple of the program, which counting rules cannot hold,
because a rule is copied whole once per adornment and a program of few
long rules called with many patterns generates few rules holding many
literals (`specs/design-planning-cost.md` section 2). The factor admits every rewrite that
specializes the program -- a handful of adornments per predicate, each with
its rules and magic rules -- and the floor admits every rewrite of a small
program outright; what it refuses is a rewrite that multiplies the program.
What a rewrite within its budgets costs to order and to hold, and the memory
limit that bounds it, is `specs/design-planning-cost.md`.

Outgrowing a budget abandons the rewrite. A warning names the budget and the
predicate being adorned, and the query is then answered from the whole
program's fixed point, exactly as `-X no-magic` answers it:

```text
relic: warning: the query's Magic rewrite outgrew its budget of 784 generated literals while adorning p/8; answering it from the whole program
```

Configuring either limit replaces both budgets rather than joining
them, so a caller who sets one gets precisely the limit asked for: a larger
rewrite than the budgets admit if the limit is larger, and a failed query
when it is exceeded. The literal budget is lifted with the other because
the budgets are proportional to the program while the cost of answering
without the rewrite is proportional to the data: ten short rules over an
8-ary predicate and a million facts rewrite to a few thousand literals,
many times the program and trivially cheap, where the whole program's
fixed point is not. A caller who sets a Magic limit has sized the rewrite,
and the library does not overrule them.

The budget bounds the rewritten program, not what it derives. Adornments of
one predicate with different bound columns each derive their own relation,
and on a program whose calls bind many patterns those relations together can
hold several times the tuples of the whole program's fixed point, though never
more rules than the budget admits.

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
an internal error instead. The check covers an aggregate literal, whose group
predicate is read completely as a negated one is: one over a group predicate
not read completely would fold an empty relation, and an aggregate edge,
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
rewrite is abandoned over its budget leaves the whole program's fixed point
instead, as a query under `-X no-magic` does. The price is that each
rewritten query derives again what it reads completely, seeding its derived
predicates with their facts again. `TODO.md` records keeping complete
relations across queries as an optimization.

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
* A rewrite abandoned over its budget shows no rewrite heading. Its phase 1
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
tables are identifier tables over the rewrite's arrays. All of it is released
when the query has run, successfully or not, as is the schedule of the
rewritten program. Symbol and predicate entries for generated names belong to
the context, as every interned entry does.

The rewrite's arena is charged to `max_memory`, per
`specs/design-planning-cost.md` section 6: the budgets of section 8 bound
the rewrite's size in literals to a multiple of the program's, and the
memory limit bounds what that multiple costs. The growable tables beside the arena are linear in
the rules the budget admits and are not charged.

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
* **Demand through an aggregate.** A group predicate is read completely,
  as a negated predicate is, and is never adorned; adorning it on its
  shared columns is left with demand through negation,
  `specs/design-aggregates.md` section 20.
* **Adorning a predicate no rule derives.** Section 3.
* **A guarded copy of each fact.** Section 6.
* **A supplementary predicate before the first call binding a column.** The
  classic construction cuts there too, which would materialize a relation
  for every adorned rule, `path :- edge, path` included, to save repeating a
  prefix that is usually one literal.
* **A threshold on cutting.** Section 6.
* **Naming supplementary predicates by content.** Section 7.
* **Relations kept across queries.** Section 9.2.
* **A rewrite of a program already materialized.** Section 2.
