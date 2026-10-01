# relic stratification design

**Status.** Design document for the dependency analysis: the predicate
dependency graph, its strongly connected components, the strata, the rejection
of recursion through negation, and the evaluation order the analysis hands to
the evaluator. Forward-looking, like every document under `specs/`;
`TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 9, 15, 26, 29, 30, 51 and 61;
`specs/design-program.md` for the compiled rules this reads and the check it
runs inside; `specs/design-eval.md` for the evaluator that consumes the
schedule; `specs/design-memory.md` for the arenas; `specs/decisions.md` D-4,
D-6 and D-9.

---

## 1. What this document settles

* what the dependency graph is, and what an edge remembers;
* how strongly connected components are found without recursion, and in what
  order they come out;
* what a stratum is, which programs are rejected, and why the order the
  components come out in is already an evaluation order;
* what the rejection says, and which cycle it names;
* what the evaluator is handed, and what it does with it;
* when the analysis runs, what it caches, and what a later load does to that.

---

## 2. The dependency graph

A vertex is a predicate, identified by its predicate identifier. Every
predicate in the table is a vertex, whether or not anything defines it.

An edge is made by each relational body literal of each compiled rule, from the
rule's head predicate to the literal's predicate:

```text
p(...) :- q(...), not r(...), X < Y.

p -> q     positive
p -/> r    negative
```

A comparison makes no edge: it reads values and no relation.

An aggregate literal makes a third kind, the **aggregate edge**, from the
rule's head to the aggregate's generated group predicate, and the group
rule its body compiles to makes the ordinary edges of that body,
`specs/design-aggregates.md` section 7:

```text
p(...) :- q(...), N = count { s(...) }.

p -> q                 positive
p =/> agg$p$1$0$2      aggregate
agg$p$1$0$2 -> s       positive, from the group rule
```

The direction is "depends on": an edge points from what is derived to what it
is derived from. That is the direction evaluation needs, because a predicate
can be evaluated once everything its edges reach is complete.

**An edge remembers the rule and the body literal that made it.** Nothing in
the analysis needs that, and the diagnostic of section 5 needs nothing else: a
note about an edge points at the literal that created it.

Duplicate edges -- two literals over the same predicate, in one rule or in two
-- are kept. Removing them would cost a set and save nothing: Tarjan's
algorithm and the searches below are correct over a multigraph, and the
duplicates are what let every edge keep its own literal.

The graph is stored in compressed adjacency form: one array of edges grouped by
source predicate, and for each predicate the offset of its first edge. Within a
predicate, edges are in rule order and then in literal order, which is the
order the program was written in. That order is the whole of what makes the
analysis deterministic; see section 9.

A query is not part of the graph. Its rule is not in the program's rule
table, and its head names no predicate -- the query is answered into a
relation the query object owns, per `specs/design-query.md` -- so there is no
vertex for it to be, and no body reads it. A query is answered after the whole
program is complete, which is the only order any query could need.

---

## 3. Strongly connected components

Two predicates share a component when each depends, directly or not, on the
other. A component is **recursive** when it has more than one member, or when
its single member has an edge to itself. Every other component is a predicate
none of whose rules reads it.

Components are found by Tarjan's algorithm, **written iteratively with an
explicit stack**. The recursive form is shorter and is a stack overflow waiting
for a generated program with a long dependency chain: the depth of its
recursion is the length of the longest path in the graph, which a program
controls. The iterative form keeps, for each vertex on the search path, the
vertex and the position of the next edge to follow. Descending pushes a frame
and backtracking pops one, so the search needs memory proportional to the
number of predicates and C stack independent of it.

**Tarjan's algorithm completes a component only after every component it can
reach.** With edges pointing along "depends on", that is exactly the order in
which components can be evaluated: when a component completes, everything it
depends on has completed before it. The components are numbered in the order
they complete, and that numbering is a topological order of the condensation
with dependencies first. No separate topological sort is needed.

The search starts from each vertex in identifier order, which is the order of
first mention in the program, and follows each vertex's edges in the order of
section 2. The order components complete in is therefore a function of the
program text alone.

---

## 4. Strata

A program is **stratified** when no negative edge has both ends in the same
component. When one does, the predicate depends negatively on something that
depends on it, which is recursion through negation, and the program is
rejected, per architecture section 9.

**An aggregate edge counts as a negative edge** here and everywhere below: a
fold needs its group relation complete, as an absence test needs its
relation complete, so recursion through an aggregate is rejected as
recursion through negation is, `specs/design-aggregates.md` section 4.7.

A stratified program gets the least stratification: the stratum of a component
is zero when it has no edge to another component, and otherwise the largest,
over its edges to other components, of

```text
stratum(target)          for a positive edge
stratum(target) + 1      for a negative or an aggregate edge
```

Every negative and aggregate edge then points strictly downward and every
positive edge points to the same stratum or lower, which is the definition
architecture section 30 gives. The computation is one pass over the components in the order
they completed: an edge leaving a component always reaches one that completed
earlier, so its stratum is already known when it is read.

**The evaluation order is the components sorted by stratum, stably.** Sorting
by stratum groups each stratum together, as architecture section 29 asks.
Stability keeps each stratum in completion order. The result is still an order
in which every component follows everything it depends on: a component's
stratum is never below that of anything it depends on, so a dependency is
either in a lower stratum, which comes first, or in the same one, where the
completion order already puts it first.

---

## 5. The unstratified diagnostic

A bare "program is not stratified" would be correct and useless: the reader
would have to find the cycle themselves, in a program large enough to have
one by accident. The diagnostic names the cycle, one edge per note, each note
pointing at the literal that creates that edge:

```text
policy.dl:18:1: error: program is not stratified
policy.dl:20:5: note: ready/1 depends negatively on blocked/1
policy.dl:25:5: note: blocked/1 depends on ready/1
```

The error points at the start of the rule that closes the cycle: the rule
holding the negative literal. The notes walk the cycle from that literal back
to the rule's head.

**Which cycle.** The negative edge is the first one, in rule and literal order,
whose ends share a component. From its target, a breadth-first search inside
the component finds the shortest path back to its source, following edges in
the order of section 2. The negative edge followed by that path is a cycle, and
it is the shortest one through that edge. When the edge is a self-loop --
`p(X) :- q(X), not p(X)` -- the cycle is the edge alone and there is one note.
Other negative edges may lie on the path, and each note says which kind of edge
it describes.

**One error per offending component.** Several negative edges inside one
component are usually one mistake seen from several places, so the component
is reported once, through its first negative edge. Distinct components are
distinct mistakes and each gets its error, in the order of their first
negative edges.

**Through an aggregate.** The closing edge is the first negative or
aggregate edge whose ends share a component, in rule and literal order over
the rules the program states; a group rule's own negative edge is never
chosen, since every cycle through a group predicate passes through the
aggregate edge that reaches it, so the error points at the rule holding the
aggregate. On the cycle every aggregate edge is followed by an edge of its
group rule, and the pair is reported as one note, keeping the group edge's
polarity and pointing at the literal inside the braces that continues the
cycle. No generated name appears in a note, `specs/design-aggregates.md`
section 7:

```text
paths.dl:9:1: error: program is not stratified
paths.dl:9:36: note: shortest/3 depends on dist/3 through an aggregate
paths.dl:12:5: note: dist/3 depends on shortest/3
```

Predicates are named `name/arity`, shortened as every predicate in a message
is.

---

## 6. The schedule

What the analysis hands to the evaluator is a **schedule**: the program's
components in evaluation order, each carrying

* its stratum;
* whether it is recursive;
* its members, in identifier order;
* the rules whose head it contains, in program order, as indexes into the
  program's rule table.

A component with no rules is left out. Such a predicate has facts alone, is
undefined, or was interned by a query's Magic rewrite; there is nothing to
run for it, and a predicate of facts alone is read as its base store holds
it (`specs/design-facts.md` section 3). The schedule is built from rules,
and a program's facts are in none of its components: loading facts adds
nothing to the dependency graph, the strata or the rule counts. Every rule of
the program belongs to exactly one component, the one containing its head,
so the schedule lists each rule exactly once.

A group predicate is derived by its one group rule and read by nothing but
the aggregate that folds it, so in a stratified program it is a component of
its own, never recursive, in the stratum its body's edges give it and
strictly below every rule holding the aggregate,
`specs/design-aggregates.md` section 7. A query's group rules belong to the
query, as its own rule does, and are in no component.

Rules are held as indexes rather than copies or pointers. A copy of each rule
would double the memory of a program of a million rules, and a pointer into the
rule table would dangle the moment a load grew it. An index stays valid for as
long as the table holds the rule, which is the life of the context.

---

## 7. Evaluation by component

The evaluator runs the schedule's components in order:

```text
for each component, in schedule order:
	seed its members with the facts their base stores hold
	if it is recursive:
		run its rules to a fixed point
	else:
		run each of its rules once
```

**A component's rules read only complete relations or its own.** A body
literal over another component reaches one earlier in the schedule, which has
already reached its fixed point and will not change again. That is what makes
one pass enough for a non-recursive component -- its rules read nothing they
write -- and what makes a fixed point over the component's rules alone the
same as one over the whole program.

It is also what will make negation meaningful. A negated literal points to a
strictly lower stratum, so its relation is complete before any rule that tests
it runs, and absence is absence rather than "not yet".

The fixed point run for a recursive component is the naive one or the
semi-naive one; `specs/design-eval.md` specifies both, and both run inside
this loop.

---

## 8. When the analysis runs, and what it caches

The analysis is part of `relic_check()`, per architecture section 51. It runs
after every pending statement has been compiled, and only then, because its
answer depends on every rule: a check that stopped early for want of memory
does not analyse a program it has not finished reading.

**The schedule describes a number of rules, and is current when that number is
the program's.** Rules are only ever appended, so a count is enough: a check
that compiled no new rule finds the schedule current and does nothing. A check
that did rebuilds the analysis from scratch, over every rule. The rebuild is
linear in the size of the program, and incremental maintenance of components
would be a great deal of mechanism to save a linear pass that happens once per
load that adds a rule; a load of facts alone leaves the schedule current.

**The analysis runs even when some statement has been rejected.** A rejected
statement never reaches the rule table, so the graph is the graph of the rules
that were accepted. A cycle through negation among those is a real mistake in
the program -- adding rules can only add edges, and can never break a cycle --
so reporting it in the same run as an unsafe rule saves the reader a second
round.

**Rejection is sticky, and reported once.** A negative edge inside a component
stays inside one however many rules are added, so a program found unstratified
stays unstratified. The program records that, is refused by every later check,
and is not analysed again: repeating the diagnostic would tell the reader
nothing new. The cost is that a second, independent cycle introduced by a
later load is not reported until the first is fixed. The command-line tool
loads every file before it checks and so reports every cycle in one run; only
an embedder that checks between loads can see the difference.

**Running out of memory leaves the old schedule in place.** The new schedule is
built completely before it replaces the old one, so a failure part way leaves
the previous schedule, which describes fewer rules than the program has and is
therefore not current. The check reports the failure and returns -1 without
marking the program invalid, and the next check tries again.

The evaluator reads the schedule only after a check has succeeded, and a check
succeeds only when the schedule is current. The evaluator confirms the count
all the same, once per materialization, because the alternative to confirming
it is running a schedule that silently leaves rules out.

**The analysis is also run on the rules a query's Magic rewrite generates**,
into a schedule of the rewrite's own that lives for one query run, per
`specs/design-magic.md` section 9.1. The analysis is the same function over a
different table of rules; the graph's edges name rules of whichever table it
was built from. The generated rules are stratifiable by construction, so there
the analysis is a guard, and a cycle it found would be an internal error.

---

## 9. Determinism

Per D-4, the analysis depends on the program and nothing else. Vertices are
visited in identifier order, which is first-mention order; edges in rule and
literal order; components complete in the order the search finds them; the
sort by stratum is stable; the cycle a diagnostic names is found by a
breadth-first search in the same edge order. No hash table is iterated and no
pointer is compared.

The order is determined by the source, not independent of it. Two programs
holding the same rules in a different order have the same components, the same
members and the same strata, and each schedule is an evaluation order, but two
components free to run in either order may run in different orders in the two.
Making the order independent of the text would mean ordering by predicate name,
which buys nothing the evaluator can use and departs from first-mention order
everywhere else.

---

## 10. Memory

The graph and the search state are scratch: they are allocated from an arena
created for one analysis and released when it ends, successfully or not.

The schedule is allocated from an arena of its own, owned by the program and
reset when a new schedule replaces it. An arena rather than the program arena,
because the program arena lives as long as the program and each rebuild would
otherwise leave the previous schedule behind in it; an arena
rather than individually allocated arrays, because the schedule is built once,
read many times and discarded whole, which is what an arena is for.

Neither counts against the relation limits of architecture section 63: they are
proportional to the program, which the caller chose, and not to what the
program derives.

---

## 11. What is deliberately absent

* **A recursive search.** Section 3.
* **Incremental maintenance of components.** Section 8.
* **Edge deduplication.** Section 2.
* **An order independent of the text.** Section 9.
* **Negation at evaluation.** The analysis guarantees that a negated relation
  is complete when it is read; the `ANTI_MEMBER` operation that reads it is
  specified in `specs/design-plan.md` and `specs/design-eval.md`.
