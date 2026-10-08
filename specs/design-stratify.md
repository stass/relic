# Analysis

The dependency analysis is the pipeline stage between compiling rules and
evaluating them. It builds the signed predicate dependency graph of a
table of compiled rules, finds its strongly connected components, rejects
recursion through negation or through an aggregate with a diagnostic that
names the cycle, assigns strata, and hands the evaluator a **schedule**: the
components that have rules, in an order in which each follows everything it
depends on. It runs inside [the check](design-frontend.md#the-check), after
every pending statement has compiled, and again on the rules each query's
Magic rewrite generates. What stratification means for a program is in
[the language reference](language.md#stratification); how the schedule is
run is in [evaluation](design-eval.md).

Related: [architecture: analysis](architecture.md#analysis),
[front end](design-frontend.md), [evaluation](design-eval.md),
[aggregates](design-aggregates.md), [Magic Sets](design-magic.md),
[facts](design-facts.md).

## Model

```text
rule table -> dependency graph -> components (Tarjan) -> rejection
                                                      -> strata -> schedule
```

The analysis is a function of a rule table and nothing else: the program's
table, or the table a query's rewrite builds. `lib/depgraph.c` builds the
graph and its components; `lib/stratify.c` rejects, assigns strata and
builds the schedule.

## Invariants

* Components are numbered in completion order, which is a topological
  order of the condensation with dependencies first.
* In a stratified program every negative and aggregate edge points to a
  strictly lower stratum, and every positive edge to the same or a lower
  one.
* The schedule lists every rule of its table exactly once, in the component
  holding the rule's head, and lists no component without rules.
* The schedule is current exactly when the number of rules it describes
  equals the table's.
* A program found unstratified stays unstratified.

## The dependency graph

A vertex is a predicate, identified by its predicate identifier; every
predicate in the table is a vertex, whether or not anything defines it.
Each relational body literal of each rule makes an edge from the rule's
head to the literal's predicate. The direction is "depends on", which is
what evaluation needs: a predicate can be evaluated once everything its
edges reach is complete. The graph is **signed**, with three kinds of edge:

```text
p(X) :- q(X), not r(X), N = count { s(X, Y), t(Y) }, M = count { u(X, Z) }.

p -> q              positive
p -/> r             negative
p =/> agg$p$1$0$3   aggregate, to the group predicate
agg$p$1$0$3 -> s    positive, from the group rule
agg$p$1$0$3 -> t    positive, from the group rule
p =/> u             aggregate, to the atom folded in place
```

The group predicate is named after the rule and the aggregate's place in
it: `agg$p$1$0$3` is the group of the aggregate at body literal 3
(literals count from 1) of rule 0 (from 0) among those deriving `p/1`.

A comparison makes no edge: it reads values, not a relation. An aggregate
makes an **aggregate edge** to the predicate whose relation the fold
reads: the generated group predicate when its body compiles to a group
rule, whose own edges then lead to what the body reads, or the atom's own
predicate when the body is one atom of distinct variables folded in place
([aggregates](design-aggregates.md)). Negative and aggregate edges are
together called **strict**: a fold needs its relation complete just as an
absence test does, so both are stratified alike.

**An edge remembers the rule and the body literal that made it.** The
analysis itself does not need that; the diagnostic needs nothing else.

**Duplicate edges are kept.** Removing them would cost a set and save
nothing: Tarjan's algorithm and the searches below are correct over a
multigraph, and duplicates let every edge keep its own literal.

The graph is stored in compressed adjacency form, built in two passes over
the rules: the first counts each predicate's outgoing edges, which fixes
where its edges begin, the second places them. Within a predicate, edges
are in rule order then literal order, the order the program was written
in, and that order is the whole of what makes the analysis deterministic.

A query is not in the graph. Its rule is in no rule table and its head
names no predicate, so there is no vertex for it and nothing reads it. A
query is answered after everything it reads is complete, the only order
any query could need.

## Strongly connected components

Two predicates share a component when each depends, directly or not, on
the other. A component is **recursive** when it has more than one member or
its single member has an edge to itself.

Components are found by **Tarjan's algorithm written iteratively with an
explicit stack**. The recursive form is shorter, but its depth is the
length of the longest dependency path, which the program controls, so a
generated program with a long chain would overflow the C stack. The
iterative form keeps the search path as an array of frames, each a vertex
and the position of the next edge to follow; descending pushes and
backtracking pops, so memory is proportional to the number of predicates
and the C stack stays flat. A vertex is on the pending stack exactly when it
has been discovered and not assigned, so no separate on-stack flag is kept.

**Tarjan's algorithm completes a component only after every component it
can reach.** With edges pointing along "depends on", that is exactly an
evaluation order: when a component completes, everything it depends on has
completed before it. Numbering components in completion order therefore
yields a topological order of the condensation, dependencies first, with no
separate topological sort. The search starts from each vertex in
identifier order, which is first-mention order, and follows edges in the
stored order, so the completion order is a function of the program text.

## Rejection and strata

A program is **stratified** when no strict edge has both ends in one
component. One that does depends negatively, or through a fold, on
something that depends on it, and the program is rejected. The strict
edges are read from the graph -- the one definition of what a dependency is
and which are strict -- so the rejection and the strata cannot disagree
about an edge.

A stratified program gets the least stratification. A component's stratum
is zero when it has no edge leaving it, and otherwise the largest, over its
edges to other components, of the target's stratum for a positive edge and
the target's stratum plus one for a strict edge. One pass in completion
order computes it, because an edge leaving a component always reaches one
that completed earlier and is already settled. Edges inside a component
are all positive at this point, since a strict one was rejected.

## The cycle diagnostic

The diagnostic names a cycle, one edge per note, each pointing at the
literal that made the edge; its form and location contract are in
[stratification](language.md#stratification). The error points at the rule
holding the **closing edge**, and the notes walk the cycle from that edge's
literal back to the rule's head.

**Which cycle.** For each offending component the closing edge is the first
strict edge inside it in rule and literal order, among rules the program
states. From its target, a breadth-first search inside the component, following
edges in stored order, finds the shortest path back to its source. The closing
edge followed by that path is the shortest cycle through that edge; it need not
be the shortest cycle in the component, since the edge is chosen first by
written order. A self-loop -- `p(X) :- q(X), not p(X)` -- is a cycle of one
edge and one note. Other strict edges may lie on the path, and each note says
which kind of edge it describes.

**One error per offending component.** Several strict edges inside one
component are usually one mistake seen from several places, so each
component is reported once, through its closing edge. Distinct components
are distinct mistakes, each reported, in the written order of their
closing edges. Every allocation the reporting needs is made before the
first report, so an analysis that runs out of memory has said nothing.

**Through an aggregate.** A group rule's own strict edge is never chosen as
the closing edge: every cycle through a group predicate enters it by the
aggregate edge that reaches it, which belongs to a written rule, so the
error points at the rule holding the aggregate. On the path, an aggregate
edge to a group predicate followed by an edge of its group rule is reported
as one note, keeping the group edge's polarity and pointing at the literal
inside the braces that continues the cycle; an aggregate edge to an atom
folded in place is one note by itself, pointing at that atom, and reads the
same. A vertex is known to be a group predicate by its predicate's kind,
which the compiler set when it named it, never by being an aggregate edge's
target, which an ordinary predicate can be. So no generated name appears
in a note; predicates are named `name/arity`, shortened as in every
message.

## The schedule

The schedule is the program's components in evaluation order, each with
its stratum, whether it is recursive, its members in identifier order, and
the rules whose head it holds, in program order, as indexes into the rule
table.

**The order is the components sorted by stratum, stably.** Sorting groups
each stratum together; stability keeps completion order within a stratum.
The result is an order in which every component follows what it
depends on: a dependency's stratum is never above its dependant's, so it is
either in a lower stratum, which comes first, or in the same one, where
completion order already puts it first. The sort is a counting sort over
strata.

**A component with no rules is left out.** Such a predicate has facts
alone, is undefined, or was generated by a query's Magic rewrite; there
is nothing to run, and a predicate of facts alone is read
from its base store ([facts](design-facts.md#the-read-and-write-funnel)).
Loading facts therefore adds nothing to the graph, the strata or the rule
counts. Every rule belongs to exactly one component, the one holding its
head, so the schedule lists each rule exactly once. A group predicate is
derived by its one group rule and read only by the aggregate that folds
it, so in a stratified program it is a component of its own, never
recursive, strictly below every rule holding the aggregate. A query's group
rules belong to the query and are in no component.

**Rules are held as indexes**, not copies or pointers. A copy of each rule
would double the memory of a large program, and a pointer into the rule
table would dangle when a load grew it. An index stays valid as long as the
table holds the rule, which is the life of the context.

## When the analysis runs

The analysis runs in the check after every pending statement has compiled,
and only then, since its answer depends on every rule; a check that stopped
early for memory does not analyse a program it has not finished reading.

**The schedule is current when it describes as many rules as the table
holds.** Rules are only appended, so a count suffices: a check that
compiled no new rule finds the schedule current and does nothing, and a
load of facts alone leaves it current. A check that did compile rules
rebuilds the analysis from scratch. The rebuild is linear in the program
and happens once per load that adds rules; incremental maintenance of
components would be a great deal of mechanism to save that pass.

**The analysis runs even when some statement was rejected.** A rejected
statement never reaches the rule table, so the graph is that of the
accepted rules, and a cycle among them is a real mistake -- adding rules
only adds edges and can never break a cycle -- so reporting it in the same
run as an unsafe rule saves the reader a second round.

**Rejection is sticky and reported once.** A strict edge inside a component
stays inside one however many rules are added, so a program found
unstratified is marked invalid, refused by every later check and never
analysed again; repeating the diagnostic would tell the reader nothing new.
The cost is that a second, independent cycle added by a later load is not
reported until the first is fixed. The command-line tool loads every file
before it checks and so reports every cycle in one run; only an embedder
checking between loads can see the difference.

The evaluator reads the schedule only after a successful check, which
guarantees it is current, and confirms the count once per evaluation all
the same, because the alternative to confirming is running a schedule
that silently leaves rules out.

**The rewrite's rules are analysed by the same function** into a schedule
of the rewrite's own that lives for one query run; the graph's edges name
rules of whichever table it was built from. The generated rules are
stratifiable by construction ([Magic Sets](design-magic.md)), so there the
analysis is a guard, and a cycle it found is an internal error.

## Failure paths and limits

* **Running out of memory leaves the old schedule in place.** The new
  schedule is built completely before it replaces the old one, so a failure
  part way leaves the previous schedule, which describes fewer rules than
  the table and is therefore not current. The check reports the failure
  without marking the program invalid, and the next check tries again.
* The graph and search state live in a scratch arena created for one
  analysis and released when it ends, successfully or not. The schedule
  lives in an arena of its own, owned by the program and replaced whole by
  a rebuild; the program arena would keep every superseded schedule for
  the life of the context.
* Neither arena is charged to the memory limit: both are proportional to
  the program the caller wrote, not to what it derives.
* Nothing is bounded but memory; the analysis is linear in rules and
  literals.

## Determinism

The analysis depends on the rule table and nothing else. Vertices are
visited in identifier order, which is first-mention order; edges in rule
and literal order; components complete in the order the search finds them;
the sort by stratum is stable; the cycle a diagnostic names is found by a
breadth-first search in the same edge order, and offending components are
reported in written order. No hash table is iterated and no pointer is
compared.

The order is determined by the source, not independent of it. Two programs
holding the same rules in different orders have the same components,
members and strata, and each schedule is an evaluation order, but two
components free to run in either order may run in different orders in the
two. Making the order independent of the text would mean ordering by
predicate name, which buys nothing the evaluator can use and departs from
first-mention order everywhere else.

## Alternatives not taken

* **A recursive search.** Its depth is controlled by the program.
* **Incremental maintenance of components.** A linear rebuild per load
  that adds rules is cheaper than the mechanism.
* **Edge deduplication.** It costs a set and loses each edge's literal.
* **An order independent of the text.** See Determinism.
* **Reporting every cycle of a component.** One mistake seen from several
  places would produce several errors.
