# relic - Small General-Purpose Datalog for C

**Status:** Engineering specification v0.2, derived from `initial-spec.md` (Draft v1)
**Tool:** `relic`
**Library:** `librelic`, public header `<relic.h>`
**License target:** 2-clause BSD
**Implementation language:** ISO C11
**Build system:** GNU autoconf / automake / libtool
**Primary platforms:** FreeBSD, OpenBSD, NetBSD, macOS
**Secondary platform:** Linux, glibc and musl
**Implementation plan:** `relic-plan.md`
**Code style:** `specs/style.md`

This document describes the target design of the system, not its current
implementation status. For what exists today see `STATUS.md`; for open work see
`TODO.md`. Changes relative to Draft v1 are listed in section 76.

---

# 1. Summary

`relic` is a small, dependency-free Datalog implementation intended to occupy the space between tiny educational interpreters and large research/database systems.

The core design is:

* conventional function-free Datalog;
* bottom-up relational evaluation;
* semi-naive evaluation for recursion;
* stratified negation;
* query-directed Magic Sets optimization;
* simple relational join optimization and indexing;
* one Unix-style command-line tool;
* one embeddable C library;
* no runtime dependency beyond libc;
* a portable autoconf/automake build needing only a C11 compiler and make;
* BSD APIs and headers used where appropriate.

The design goal is not to implement Prolog.

The design goal is:

> A small relational inference engine that feels like `awk`, `sqlite3`, or `grep`: easy to install, easy to embed, predictable, scriptable, and sufficiently general to be useful for dependency analysis, policy, static analysis, configuration, graph queries, project tracking, and other small-to-medium deductive workloads.

A typical program should look like:

```prolog
edge(a, b).
edge(b, c).
edge(c, d).

path(X, Y) :-
    edge(X, Y).

path(X, Z) :-
    edge(X, Y),
    path(Y, Z).

path(a, X)?
```

Invocation:

```sh
relic graph.dl
```

or:

```sh
relic -q 'path(a, X)?' graph.dl
```

The implementation should remain comprehensible enough that a systems programmer can understand the evaluator without studying a full logic-programming runtime.

---

# 2. Product Goals

## 2.1 Primary goals

`relic` shall:

1. Implement a useful, conventional subset of Datalog with well-defined semantics.
2. Correctly evaluate recursive rules using semi-naive bottom-up evaluation.
3. Support safe stratified negation.
4. Use Magic Sets to specialize evaluation for bound queries.
5. Represent predicates internally as relational tables rather than Prolog-like clauses.
6. Support efficient joins using indexes and basic join ordering.
7. Operate as both:

   * a command-line program; and
   * a reusable C library.
8. Require no external runtime, VM, parser generator, database, or
   third-party library. The sole optional dependency is `libedit`, used
   for REPL line editing when `configure` finds it.
9. Build naturally on BSD systems using BSD libc facilities.
10. Remain small enough that the entire engine is realistically auditable.

## 2.2 Size goal

The target is approximately:

```text
parser / lexer             600–900 LOC
symbols / values           400–600
relations / indexes        700–1000
rules / analysis           500–700
stratification             250–400
Magic Sets                 500–800
planner                    400–700
semi-naive evaluator       600–900
CLI / formatter            400–600
compatibility layer        100–300
-----------------------------------
core                      ~4–7 KLOC
```

Tests are not included in this budget.

Code size is a design constraint, not merely a statistic.

Features requiring disproportionately large machinery should normally be omitted.

---

# 3. Non-goals

Version 1 shall not implement:

* Prolog;
* structures or compound function terms;
* lists as a special language construct;
* cut;
* clause-order execution semantics;
* backtracking as an externally visible execution model;
* dynamic predicates;
* arbitrary foreign-function calls;
* meta-programming;
* unstratified negation;
* well-founded semantics;
* answer-set programming;
* aggregates;
* SQL;
* transactions;
* persistent database files;
* incremental maintenance across arbitrary insertions/deletions;
* distributed execution;
* parallel execution, specified for a later stage in
  `specs/design-parallel.md`;
* cost-based optimization comparable to an SQL database.

Some of these may be added later if they preserve the character of the system.

---

# 4. Intended Uses

Representative workloads include:

```text
dependency graphs
build dependency analysis
project/task dependencies
package relationships
filesystem metadata analysis
access-control policy
firewall/policy reasoning
software architecture analysis
static-analysis facts
call graphs
reachability
configuration validation
network topology
small knowledge bases
compiler facts
test dependency analysis
Unix pipeline data processing
```

The system should perform especially well when the extensional database contains thousands to millions of tuples and the rule set contains tens to hundreds of rules.

It is not intended initially as an analytical database for billions of tuples.

---

# 5. Language

## 5.1 Facts

Facts are ground predicates:

```prolog
edge(a, b).
status(parser, done).
size(database, 120).
name(user1, "Alice").
```

Duplicate facts have no semantic effect.

Datalog relations use set semantics.

A fact is a rule with an empty body in the language. The facts a program
states are stored as the tuples of their predicate's base store rather than
as statements and compiled rules; the base store, loading and how evaluation
reads it are specified in `specs/design-facts.md`.

---

# 6. Values

Version 1 supports three value types:

```text
symbol
string
signed 64-bit integer
```

Examples:

```prolog
status(foo, active).
name(foo, "Parser cleanup").
priority(foo, 10).
```

Symbols are bare identifiers.

Strings are quoted.

Integers are decimal signed integers.

There are no floats, nulls, blobs, lists, or arbitrary structures in v1.

---

# 7. Variables
Variables begin with an uppercase ASCII letter or an underscore:

```prolog
X
Node
Dependency
_Unused
```

`_` alone is an anonymous variable.

Every occurrence of `_` denotes a distinct variable.

Example:

```prolog
edge(X, _).
```

A variable whose name begins with `_` but is longer than one character, such as
`_Unused`, is an ordinary named variable that is exempt from the
singleton-variable warning below. This follows established logic-programming
practice and lets a rule document a column it deliberately ignores.

Variable scope is one rule or query.

A variable occurring exactly once in a rule, and not beginning with `_`,
produces a warning rather than an error:

```text
project.dl:12:14: warning: variable Y occurs only once in this rule
project.dl:12:14: note: rename it to _Y to silence this warning
```

A singleton variable is the most common Datalog typo and the check is nearly
free once variable numbering exists.

---

# 8. Rules

Rules use conventional Datalog syntax:

```prolog
path(X, Y) :-
    edge(X, Y).
```

Multiple body literals form conjunction:

```prolog
blocked(X) :-
    depends(X, Y),
    unfinished(Y).
```

Recursive rules are allowed:

```prolog
path(X, Z) :-
    edge(X, Y),
    path(Y, Z).
```

Mutually recursive predicates are allowed.

---

# 9. Negation

Stratified negation uses:

```prolog
not predicate(...)
```

Example:

```prolog
ready(X) :-
    todo(X),
    not blocked(X).
```

Programs containing recursion through negation are rejected.

For example:

```prolog
a(X) :- c(X), not b(X).
b(X) :- c(X), not a(X).
```

must produce a static error.

---

# 10. Comparisons
The following built-in comparisons are supported:

```text
=
!=
<
<=
>
>=
```

Example:

```prolog
large(X) :-
    size(X, N),
    N >= 100.
```

Version 1 comparisons are filters. They do not generate new values and they do
not bind variables. Therefore:

```prolog
answer(X) :-
    X = 42.
```

is rejected as unsafe: `X` must be bound by a positive relational literal.

## 10.1 Type rules

Every value has one of three types: `symbol`, `string`, `integer`. These are
distinct domains. The symbol `foo` and the string `"foo"` are never equal.

`=` and `!=` are total. Values of different types are never equal:

```text
foo  =  "foo"     false
foo != "foo"      true
```

Ordering is defined only between values of the same type:

```text
integer   numeric order
symbol    bytewise order of the symbol spelling
string    bytewise order of the string contents
```

An ordering comparison between values of different types yields false, so the
candidate binding is filtered out. This is deliberately not an error: predicate
arguments are untyped and a column may legitimately hold more than one type.
Verbosity level 2 reports the first such comparison per rule so unintended type
mixing remains discoverable.

Bytewise ordering is used rather than locale collation so that results never
depend on the environment. See section 61.

Arithmetic expressions are explicitly deferred.

---

# 11. Queries

A query terminates with `?`:

```prolog
path(a, X)?
```

Conjunctive queries are permitted:

```prolog
depends(X, Y),
status(Y, todo)?
```

Internally, every query is transformed into a synthetic rule:

```prolog
$q0(X, Y) :-
    depends(X, Y),
    status(Y, todo).
```

The rule's head is the list of output columns and names no predicate: the
query is answered into a relation the query object itself owns, and `$q0` is
the name diagnostics and `-e` output call it by.

The query output columns are the named variables in first-occurrence order.

Anonymous variables are omitted.

Queries have set semantics.

Result order is unspecified unless sorting is explicitly requested.

The synthetic rule, its name and its checking are specified in
`specs/design-plan.md`; what the query object owns, where its answer lives
and how it is released are specified in `specs/design-query.md`.

---

# 12. Comments

A `%` begins a comment extending to the end of the line:

```prolog
% Dependency relationships
depends(parser, lexer).
```

No block comments are required in v1.

---

# 13. Grammar

Approximate grammar:

```text
program      := statement*

statement    := fact "."
              | rule "."
              | query "?"

fact         := atom

rule         := atom ":-" body

query        := body

body         := literal ("," literal)*

literal      := atom
              | "not" atom
              | comparison

atom         := identifier "(" [ term ("," term)* ] ")"

term         := variable
              | symbol
              | string
              | integer

comparison   := term comparator term

comparator   := "=" | "!=" | "<" | "<=" | ">" | ">="
```

Zero-arity predicates may be supported:

```prolog
initialized.
```

They are internally relations of arity zero.

The lexical structure, the abstract syntax tree, the interning the front end
performs, and the diagnostics and error recovery it produces are specified in
`specs/design-parser.md`.

---

# 14. Safety Rules

All rules must be range restricted.

Every variable appearing in a rule head must occur in a positive relational body literal.

Example rejected:

```prolog
foo(X) :-
    bar(Y).
```

Variables occurring in a negated literal must be bound by preceding or reorderable positive literals.

Example accepted:

```prolog
ready(X) :-
    card(X),
    not blocked(X).
```

Example rejected:

```prolog
ready(X) :-
    not blocked(X).
```

Variables used by `<`, `>`, etc. must similarly be bindable from positive relational literals.

Safety analysis occurs before evaluation.

Predicate identity, variable numbering, the compiled form of a rule, the
safety analysis and the warnings that accompany it are specified in
`specs/design-program.md`.

---

# 15. Semantic Model

Each predicate corresponds to a mathematical relation:

```text
edge/2
status/2
path/2
```

Facts define extensional relations.

Rules define intensional relations.

The meaning of a positive recursive program is its least fixed point.

Stratified programs are evaluated one stratum at a time.

All relations use set semantics.

---

# 16. Architecture

The system consists logically of:

```text
source
  ↓
lexer/parser
  ↓
AST
  ↓
semantic validation
  ↓
predicate dependency graph
  ↓
stratification + SCC analysis
  ↓
query adornment / Magic Sets
  ↓
rule planning
  ↓
semi-naive execution
  ↓
query relation
  ↓
formatter
```

The important architectural rule is:

> The evaluator operates on relations and tuples, not on a Prolog execution stack.

---

# 17. Predicate Identity
A predicate is uniquely identified by:

```text
(name, arity)
```

Therefore:

```prolog
foo(X).
foo(X, Y).
```

are unrelated predicates.

Internally:

```c
typedef struct relic_pred relic_pred_t;

struct relic_pred {
	relic_relation_t	*relation;
	uint32_t		 name;
	uint16_t		 arity;
};
```

Predicate names are interned.

A predicate may have both facts and rules. Facts seed its relation and rules
derive further tuples into the same relation.

---

# 18. Symbol Interning

All symbols and string literals should be interned.

The parser converts:

```text
"depends"
"foo"
"todo"
```

into integer IDs.

The evaluator should almost never compare C strings.

A simple open-addressed hash table should be used for symbol interning.

No external hash-table dependency should be introduced.

String bytes should live in an arena or growable string pool.

---

# 19. Value Representation
## 19.1 Public representation

The public ABI uses an explicit representation:

```c
typedef struct relic_value relic_value_t;

enum relic_type {
	RELIC_SYMBOL,
	RELIC_STRING,
	RELIC_INTEGER
};

struct relic_value {
	enum relic_type	type;
	union {
		uint32_t	id;
		int64_t		integer;
	} v;
};
```

For `RELIC_SYMBOL` and `RELIC_STRING`, `v.id` identifies an interned byte
string whose contents are obtained with:

```c
const char *
relic_string(relic_t *, uint32_t id, size_t *lenp);
```

## 19.2 Internal representation

Internally every value, integers included, is interned and is represented by
an opaque 32-bit domain identifier:

```c
typedef uint32_t relic_dom_t;		/* 0 is never a valid value */
```

A tuple is therefore a dense array of `relic_dom_t`. The evaluator compares,
hashes and joins values as plain 32-bit integers and never inspects a payload.
Only comparison operators, sorting and output formatting decode an identifier
into the value it names.

The rationale, the alternatives considered and their measured trade-offs are
recorded in `relic-plan.md`, decisions D-3 and D-24. In summary:

* joins and duplicate elimination reduce to integer work on a dense array;
* tuple storage is four bytes per column whatever the value type, which matters
  because symbol-valued columns dominate real Datalog workloads;
* a program with very many distinct integers pays eight bytes plus a tagged
  hash slot for each distinct integer, which is the representation's one real
  cost;
* one context holds at most 2^30 - 1 symbols and 2^30 - 1 distinct integers.

Domain identifiers are assigned in order of first appearance within each
type, so they are reproducible, but their numeric order carries no meaning.
Nothing may order values by domain identifier; ordering compares the values
the identifiers decode to.

The representation is internal and is reached only through a small set of
accessors, so it can later be replaced by a tagged 64-bit cell without
affecting library consumers.

The identifier's encoding -- its type in its top bits, a symbol value being
its symbol identifier, and a value table of integers only -- and the
symbol pool's entry and byte packing are specified in
`specs/design-comparison-cost.md` section 7. D-3 asks for the
representation to be revisited when a measured workload shows its cost;
section 7.1 there is that revisit, and it keeps D-3's decision of one 32-bit
interned domain and changes only the encoding behind its accessors.

---

# 20. Relation Representation
Every predicate owns a relation.

Conceptually:

```c
typedef struct relic_relation relic_relation_t;

struct relic_relation {
	relic_pred_t		*pred;
	relic_tuple_store_t	 tuples;
	relic_index_head_t	 indexes;
};
```

Tuples are stored densely as `relic_dom_t` arrays with the arity as stride.

Indexes refer to tuple identifiers rather than duplicating whole tuples.

Example:

```text
depends/2

tuple 0: parser lexer
tuple 1: planner parser
tuple 2: eval planner
```

The tuple store is append-only for the lifetime of an evaluation: a tuple
identifier, once assigned, denotes the same tuple until the database is
modified. Semi-naive evaluation depends on this, since it represents a delta as
a range of tuple identifiers rather than as a separate relation.

---

# 21. Tuple Set

Each relation requires a canonical full-tuple hash index used for:

```text
duplicate elimination
membership tests
delta insertion
```

Insertion operation:

```c
int
relic_relation_insert(..., bool *addedp);
```

sets `*addedp` true only when the tuple was newly inserted.

This property drives fixed-point detection.

Newness is an output rather than the return value because an insertion can
also fail, for memory or for a limit, and a two-valued return would conflate a
failure with a duplicate. They are opposite answers: a duplicate means the
fixed point is closer, a failure means the answer is unknown.

The storage of a relation, the tuple identifier and the append-only invariant
it carries, the canonical full-tuple index, the delta range and the tuple and
memory limits enforced against them are specified in
`specs/design-relations.md`.

---

# 22. Secondary Indexes

Indexes should be created for argument-position combinations required by execution plans.

Example query:

```prolog
depends(parser, X)
```

requires an index corresponding to:

```text
depends/2 columns {0}
```

Another rule may require:

```text
depends/2 columns {1}
```

Indexes should be built lazily or during plan preparation.

For typical low-arity Datalog predicates, an argument-position bitmask is sufficient.

Version 1 may limit predicate arity to 32.

This is intentionally generous for Datalog while allowing:

```c
uint32_t keymask;
```

to represent index keys cheaply.

What an index stores for a key, how a lookup walks it, how insertion keeps it
current and what it costs against the memory limit are specified in
`specs/design-index.md`. That a key's values are stored beside its chain,
and the tag byte its identifier table carries, are specified in
`specs/design-comparison-cost.md` section 6.

---

# 23. Rule Planning

The textual order of positive body literals is not execution order.

For:

```prolog
result(X) :-
    huge(X, Y),
    tiny(X),
    medium(Y).
```

the planner may execute:

```text
tiny(X)
huge(X,Y)
medium(Y)
```

The optimizer should prefer literals having:

1. more currently bound arguments;
2. smaller candidate relations;
3. more selective indexes.

Version 1 uses a greedy planner.

No exhaustive join-order search is required.

When a rule is planned, the estimate a literal is scored by, the tie-breaks and
the access path each literal gets are specified in `specs/design-planner.md`.
What bounds the cost of planning -- why ordering a body costs a bounded
number of passes over it rather than the square of its length, and why the
memory an evaluation's plans and a query's rewrite occupy is charged to
`max_memory` -- is specified in `specs/design-planning-cost.md`.

---

# 24. Statistics

Each relation should maintain inexpensive statistics:

```text
tuple count
distinct value count per column
available indexes
```

Exact multi-column selectivity statistics are unnecessary initially.

Planner heuristics must remain deterministic.

How distinct counts are computed and kept correct is specified in
`specs/design-planner.md` section 3.1.

---

# 25. Execution Bindings
Rule evaluation operates on variable bindings.

Each compiled rule assigns small integer identifiers to variables:

```text
X -> 0
Y -> 1
Z -> 2
```

An evaluation frame therefore resembles:

```c
typedef struct relic_binding relic_binding_t;

struct relic_binding {
	relic_dom_t	*values;
	uint64_t	 bound;
};
```

Bindings hold domain identifiers, not public values, so no conversion happens
inside a join.

A rule may use at most 64 distinct variables. This is a hard limit rather than
a fallback to a dynamic bitmap: the limit is generous for Datalog, and a fixed
word keeps the binding frame a plain array with no allocation in the join loop.
Exceeding it is a static error naming the rule.

---

# 26. Bottom-Up Evaluation

Non-recursive rules execute after their dependencies are complete.

Recursive strongly connected components use fixed-point evaluation.

Naive conceptual evaluation:

```text
repeat:
        evaluate every rule
        insert newly derived tuples
until nothing changes
```

must not be the primary recursive algorithm.

Version 1 shall use semi-naive evaluation.

---

# 27. Semi-Naive Evaluation

For every recursive relation `R`, maintain:

```text
R      all known tuples
ΔR     tuples derived during the previous iteration
ΔR'    tuples derived during this iteration
```

Consider:

```prolog
path(X, Z) :-
    path(X, Y),
    edge(Y, Z).
```

After the initial round, it is unnecessary to repeatedly join every known `path` tuple against `edge`.

Instead evaluate:

```text
Δpath ⋈ edge
```

New results enter `Δpath'`.

The process continues until all delta relations are empty.

---

# 28. Multiple Recursive Literals

For:

```prolog
p(X, Z) :-
    p(X, Y),
    p(Y, Z).
```

semi-naive planning generates delta variants so every new derivation includes at least one newly produced input.

Conceptually:

```text
Δp ⋈ p
p  ⋈ Δp
```

Duplicate results are eliminated by the destination relation.

The compiler should generate these variants automatically.

What each literal of a variant may see of its component's relations, so
that every derivation is made once, where the delta literal is placed and
how a lookup reaches a delta's start, and what a duplicate check costs, are
specified in `specs/design-evaluation-cost.md`.

---

# 29. SCC Evaluation

Construct the positive predicate dependency graph.

Compute strongly connected components.

Rules in acyclic SCCs require no fixed-point loop.

Recursive SCCs are evaluated using semi-naive deltas.

SCCs within each stratum are processed in topological order.

The dependency graph, the order components are found in, and the schedule the
evaluator is handed are specified in `specs/design-stratify.md`.

The executor, the naive and semi-naive fixed points, and how a query's answers
reach a caller are specified in `specs/design-eval.md`.

---

# 30. Stratification

For every rule:

```prolog
p(...) :-
    q(...),
    not r(...).
```

construct dependency edges:

```text
p → q    positive
p -/> r  negative
```

A valid stratification requires every negative dependency to point strictly to a lower stratum.

A negative edge inside a dependency SCC is an error.

Example diagnostic:

```text
policy.dl:18:1: error: program is not stratified
policy.dl:20:5: note: ready/1 depends negatively on blocked/1
policy.dl:25:5: note: blocked/1 depends on ready/1
```

The error points at the rule that closes the cycle, and each note at the body
literal that creates its edge. Which cycle is named, and why one error is
reported per offending component, are specified in
`specs/design-stratify.md`.

---

# 31. Negation Execution

Negation is evaluated as indexed anti-membership.

Example:

```prolog
ready(X) :-
    todo(X),
    not blocked(X).
```

Once `X` is bound:

```text
lookup blocked(X)

present     → reject binding
absent      → continue
```

Because lower strata are complete before upper strata execute, absence is semantically meaningful.

---

# 32. Magic Sets

Magic Sets are the primary query-specialization mechanism.

Without Magic Sets, a query:

```prolog
path(a, X)?
```

may cause the engine to compute:

```text
path(X,Y)
```

for all reachable pairs in the database.

Magic Sets propagate the binding `a` into recursive evaluation.

---

# 33. Adornments

For a predicate invocation, each argument is classified as:

```text
b = bound
f = free
```

Therefore:

```prolog
path(a, X)
```

has adornment:

```text
path^bf
```

while:

```prolog
path(X, Y)
```

has:

```text
path^ff
```

Different adornments are treated as separate internal specialized predicates.

They are not visible in ordinary output.

A predicate reached with every column free subsumes its other adornments,
and a rewrite left with no bound column reads the program's own relations
rather than copying them; specified in `specs/design-comparison-cost.md`
section 5.

---

# 34. Magic Seed

For:

```prolog
path(a, X)?
```

the rewrite creates a seed equivalent to:

```prolog
magic_path_bf(a).
```

Rules producing `path^bf` are guarded by this magic relation.

Conceptually:

```prolog
path_bf(X, Y) :-
    magic_path_bf(X),
    edge(X, Y).
```

Recursive demand is propagated as additional magic facts.

---

# 35. Sideways Information Passing Strategy

Magic rewriting requires an execution order describing how bindings become available.

`relic` uses the same greedy ordering algorithm used by the relational planner.

Given:

```prolog
p(X) :-
    a(X, Y),
    b(Y, Z),
    c(Z).
```

and `X` initially bound:

```text
magic p(X)
   ↓
a(X,Y)       binds Y
   ↓
b(Y,Z)       binds Z
   ↓
c(Z)
```

Magic predicates are generated according to this flow.

This prevents the Magic Sets subsystem from inventing an independent notion of body ordering.

---

# 36. Magic Sets and Recursion

Recursive query:

```prolog
path(X, Z) :-
    edge(X, Y),
    path(Y, Z).
```

queried as:

```prolog
path(a, X)?
```

propagates demand approximately as:

```prolog
magic_path(a).

magic_path(Y) :-
    magic_path(X),
    edge(X, Y).
```

The rewritten program is then evaluated using the normal semi-naive engine.

Magic Sets therefore do not require a separate execution engine.

They are a source-to-source/internal-rule transformation.

---

# 37. Magic Sets and Negation

Version 1 deliberately uses conservative semantics around negation.

Magic propagation occurs through positive dependencies.

A relation referenced negatively is evaluated completely in its already-lower stratum.

For example:

```prolog
ready(X) :-
    card(X),
    not blocked(X).
```

`blocked/1` may be fully materialized before specialized evaluation of `ready/1`.

This keeps stratified negation straightforward and avoids complex magic transformations across negative edges.

Future versions may optimize this further if correctness remains obvious.

---

# 38. Magic Rewrite Termination

Adornment generation must be memoized.

A unique key consists of:

```text
predicate
arity
binding pattern
```

There are at most:

```text
2^arity
```

adornments for one predicate.

For realistic Datalog arities this is naturally finite, but finite is not
small: a program whose rules each ask for one more bound column reaches every
one of them. The rewrite is therefore bounded in proportion to the program it
rewrites. A caller that configures a Magic limit gets that limit, per section
63, and no budget; one that configures neither gets budgets, and a rewrite
outgrowing one is abandoned with a warning and the query answered from the
whole program. The rewrite is an
optimization and must not decide whether a query is answered.

Duplicate rewritten rules must be structurally deduplicated.

Which predicates a query reads completely, the rules the rewrite generates,
its limits and its explain output are specified in `specs/design-magic.md`.

---

# 39. Query Execution Modes

Two execution modes exist.

### Query-directed mode

Default for explicit queries.

Pipeline:

```text
query
→ synthetic query relation
→ adornment
→ Magic Sets
→ plan
→ semi-naive evaluation
→ answers
```

### Full materialization mode

Requested explicitly when the caller wants all derived relations.

Pipeline:

```text
program
→ stratification
→ plan
→ semi-naive materialization
```

Magic Sets are bypassed.

---

# 40. CLI
The executable is:

```text
relic
```

No separate compiler is required for v1.

Utility syntax follows traditional BSD command-line conventions: single-letter
options parsed with `getopt(3)`. There are no long options.

Usage:

```text
relic [-aceinsvV] [-F format] [-I import] [-j threads] [-l limit=value]
      [-o file] [-q query] [-X debug] [file ...]
```

Options, in the alphabetical order used by the usage message and the manual
page:

```text
-a          materialize the complete program; do not specialize queries
-c          check syntax, safety and stratification; do not evaluate
-e          print the selected execution plan and Magic rewrite
-F fmt      output format: text, csv, tsv, json
-I import   add facts from a csv or tsv table; repeatable
-i          enter interactive mode once the queries have run
-j threads  share evaluation among this many threads; 0: one per processor
-l limit=n  set a resource limit; repeatable
-n          suppress normal query output; useful with -e or -v
-o file     write query output to file instead of stdout
-q expr     execute expr as a query; repeatable
-s          sort query results before printing
-v          increase diagnostic verbosity; repeatable
-V          print version and exit
-X debug    enable an internal behaviour; repeatable
```

Resource limits, where 0 asks for the default -- no limit on tuples or
memory, and the budget of section 63 on the Magic rewrite:

```text
-l tuples=N        maximum tuples across all relations
-l memory=N        maximum bytes of relation and index storage, plus
                   the working state of an evaluation or a rewrite
-l magic-preds=N   maximum generated adorned and magic predicates
-l magic-rules=N   maximum generated rules
```

A single repeatable `key=value` option is used rather than one letter per knob,
so that new limits never consume new option letters. Values are validated for
range and trailing characters.

`-X` selects internal behaviour for testing and debugging. It is documented in
relic(1) but is not part of the stable interface:

```text
-X naive        use the naive fixed point instead of semi-naive evaluation
-X no-index     disable secondary indexes
-X no-magic     disable Magic Sets for explicit queries
-X no-reorder   evaluate body literals in textual order
-X no-stream    gather every answer before writing its first row
-X parallel     defer every round's insertions to its end, cut finely
-X stats        print evaluation counters to stderr on exit
```

`-X` exists so the differential test suite can compare evaluation strategies
through the ordinary command-line interface rather than through a private test
entry point. Each strategy must produce identical answers; see section 66.

Files are operands rather than arguments to a file option:

```sh
relic rules.dl facts.dl
```

If no files are given and standard input is not a terminal, the program is read
from stdin, unless the interactive session or an import of `-` reads it.

An import names a table's format, whether it has a header, the predicate its
records become facts of, and a type for each column, and it loads after the
program's files, in the order given. Its operand, the formats and the rules for
standard input are specified in `specs/design-import.md`.

---

# 41. CLI Examples

Validate:

```sh
relic -c project.dl
```

Run queries embedded in a file:

```sh
relic project.dl
```

Supply an external query:

```sh
relic -q 'ready(X)?' project.dl
```

Query CSV output:

```sh
relic -F csv -q 'depends(X, Y)?' project.dl
```

Inspect optimization:

```sh
relic -e -q 'path(a, X)?' graph.dl
```

Answer a program's queries over facts imported from a table:

```sh
relic -I tsv:edge:symbol,symbol=edge.tsv graph.dl
```

Force whole-program materialization:

```sh
relic -a graph.dl
```

Interactive use:

```sh
relic -i graph.dl
```

Pipeline:

```sh
cat graph.dl | relic -q 'path(a, X)?'
```

Bound the work a query may do:

```sh
relic -l tuples=5000000 -l memory=2147483648 -q 'path(X, Y)?' graph.dl
```

---

# 42. Interactive Mode

The REPL accepts ordinary queries directly:

```text
relic> path(a, X)?
b
c
d

relic> status(X, todo)?
parser
planner
```

Minimal commands:

```text
.help
.stats
.plan QUERY
.quit
```

No complex debugger is required.

Input history should use `libedit` only if available at compile time.

`libedit` must remain optional.

Without it, the REPL uses standard input normally.

---

# 43. Output

Default text output should be simple and shell friendly.

Single-column query:

```text
parser
planner
storage
```

Multi-column query:

```text
parser lexer
planner parser
```

No decorative table borders by default.

`-F csv` and `-F tsv` provide machine-readable tabular formats.

`-F json` emits an array of rows or streaming row arrays; the exact representation must be stable and documented.

Diagnostics always go to stderr.

Results go to stdout or the `-o` target.

The phases of an invocation and the exact form of every output format are
specified in `specs/design-cli.md`. The tool formats rows into a buffer of
its own and writes it in blocks, per `specs/design-comparison-cost.md`
section 3.

---

# 44. Exit Status

BSD/Unix conventions:

```text
0   successful execution
1   query/program error
2   command-line usage error
```

Where available, implementation may internally use `<sysexits.h>`, but public behavior should remain simple and documented.

---

# 45. BSD APIs
The implementation prefers BSD facilities where they improve simplicity.

Expected headers and APIs include:

```c
#include <err.h>
#include <sysexits.h>
#include <unistd.h>

#include "compat.h"		/* queue.h, tree.h, missing functions */
```

Use:

```text
getopt(3)
err(3), errx(3), warn(3), warnx(3)
reallocarray(3)
strtonum(3)
getentropy(2)
bzero(3), bcopy(3), bcmp(3)
```

where appropriate. `err(3)` and its variants belong to the command-line tool;
library code reports through the diagnostic interface of section 56 and never
writes to stderr on its own.

`TAILQ`, `STAILQ` and `SLIST` are used for compiler and evaluator worklists
where they simplify ownership. `RB_TREE` may be used for small ordered
structures where hashing is unnecessary.

Performance-critical relation, symbol and value lookup use purpose-built
open-addressing hash tables rather than red-black trees.

Availability of every facility named here is determined by `configure`, not by
a platform conditional in the sources. See section 46.

---

# 46. Portability Layer
Linux and other non-BSD systems lack some BSD functions, and some BSD systems
lack functions common elsewhere. Availability is decided by `configure`; it is
never decided by a platform test inside the engine.

```text
compat/
    compat.h          the single internal header that hides all of the below
    queue.h           bundled unconditionally
    tree.h            bundled unconditionally
    reallocarray.c    compiled only where reallocarray(3) is missing
    strlcpy.c         compiled only where strlcpy(3) is missing
    strlcat.c         compiled only where strlcat(3) is missing
    strtonum.c        compiled only where strtonum(3) is missing
    getentropy.c      compiled only where getentropy(2) is missing
```

`queue.h` and `tree.h` are bundled rather than probed. They are header-only and
small, and the macro set provided by the system copy differs between glibc,
musl, macOS and the BSDs in ways that are tedious to probe correctly and easy
to probe incompletely. Using one known copy everywhere removes a class of
portability defect outright and makes list and tree behaviour identical on all
platforms. Each bundled copy keeps the upstream license header it arrived
with: `queue.h` carries the 3-clause BSD notice of the Regents of the
University of California, and `tree.h` the ISC notice of its author. Both are
compatible with the 2-clause BSD license of the project. `compat/README.md`
records each copy's upstream origin, revision, checksum and the patches
applied to it.

Function-level compatibility uses `AC_REPLACE_FUNCS`, so a replacement object
is compiled only where it is needed and engine sources simply call
`reallocarray()`.

Rules:

* every `.c` file includes `config.h` first;
* `include/relic.h` never includes `config.h` and never depends on it;
* no source file outside `compat/` contains a platform conditional;
* no broad portability framework is introduced.

---

# 47. Memory Management
The engine uses arenas for structures with a common lifetime:

```text
string arena       symbol bytes, packed without alignment
program arena      rules, parsed terms
query arena        the query's compiled rule
rewrite arena      the Magic rewrite of one query's program
evaluation arena   plans and transient binding and join state
```

Relation tuples and indexes are not arena allocated. They outlive individual
queries, they must be able to shrink, and they are accounted against a limit
individually. They are growable arrays, `specs/design-memory.md` section 5,
with explicit failure handling. The rewrite and evaluation arenas are
accounted against the same limit as a whole, because what they hold is
multiplied out of the program; see `specs/design-planning-cost.md`.

Arena allocation returns `NULL` on exhaustion and sets a sticky failure flag on
the arena. Callers check the returned pointer as usual; the flag exists so a
deep helper that has already failed can return early without inventing an error
path, and so each compilation phase can confirm at its boundary that no
allocation failed. Arena exhaustion is reported as an ordinary diagnostic and a
`-1` return, never an abort: a library must not terminate its host.
The one exception is memory the library finds already corrupted. A growable
array whose sealed header no longer matches, or whose mapping the system
refuses to release, aborts the process: returning would leave the library
freeing or unmapping memory it cannot prove it owns, and whatever corrupted
the header has already broken the host.

Avoid one `malloc()` per AST node or per tuple.

The arena, the growable-array helper, the open-addressing identifier table,
the symbol pool and the value table are specified in
`specs/design-memory.md`. Arrays past a page threshold are mapped rather
than malloced, so that a growth leaves nothing resident behind it; specified
in `specs/design-comparison-cost.md` section 8.

---

# 48. C Library
Public library:

```text
librelic
```

Public header:

```text
<relic.h>
```

All major objects are opaque. Each is declared as an incomplete struct with a
matching `_t` typedef, so callers write the typedef and cannot depend on
internal layout:

```c
typedef struct relic relic_t;
typedef struct relic_query relic_query_t;
typedef struct relic_result relic_result_t;
```

Five types are complete rather than opaque, because a caller constructs or
reads each one directly and an accessor per field would buy nothing:

* `relic_value_t` of section 19, which callers build and inspect;
* `relic_location_t` of section 56, a (file, line, column) triple that every
  diagnostic callback reads;
* `relic_config_t` of section 49, which a caller fills in before creating a
  context;
* `relic_import_t` of section 50, which a caller fills in to describe a table
  to import;
* `relic_stats_t`, the context's counters, which a caller reads.

```c
void
relic_stats(relic_t *, relic_stats_t *);
```

fills a `relic_stats_t` with counters of the work evaluation has done --
fixed-point iterations, derivations, insertions, tuples scanned and looked
up, rounds shared among threads -- and of what
the context holds: tuples, relation and index storage, and distinct values by
type. `-X stats` and the interactive `.stats` print it. The counters are a
fixed, small set read at one moment, so a struct is their natural shape; a
callback suits rows and diagnostics because those are unbounded in number and
arrive while work is in progress. What the counters count depends on the
evaluation strategy and is not a stable interface. The struct's layout is part
of the ABI, so adding a counter is an interface change, and the library's
version-info of section 65.6 changes with it.

The header is strict C11, contains no project-internal includes, does not
include `config.h`, and is guarded for C++ inclusion. Only symbols matching
`^relic_` are exported from the shared library.

The interface as a whole -- value constructors, programmatic facts, the
collector, what a callback may call, the symbol audit and versioning -- is
specified in `specs/design-api.md`.

---

# 49. Library Lifecycle
Representative API:

```c
relic_t *
relic_create(void);

void
relic_destroy(relic_t *);
```

Configuration variant:

```c
struct relic_config {
	size_t		max_memory;
	size_t		max_tuples;
	size_t		max_magic_preds;
	size_t		max_magic_rules;
	unsigned int	flags;
	size_t		threads;
};

relic_t *
relic_create_config(const relic_config_t *);
```

A zero `max_memory` or `max_tuples` means unlimited; a zero Magic limit means
the budget of section 63. The limits correspond one to one with the
`-l key=value` options of section 40 and with the limits of section 63, so a
limit exists in exactly one place and the tool merely parses into it.

`threads` is the size of the worker pool the context owns, the calling
thread included, which the tool's `-j` sets; zero and one both mean the
calling thread alone, and more than `RELIC_THREADS_MAX` is refused when the
context is created. The pool's threads start the first time an evaluation
shares a round and run only inside an operation the caller started, so a
context is still used by one caller thread at a time. Answers, relations,
diagnostics and every counter but `parallel_rounds` are the same at every
thread count (`specs/design-parallel.md`).

`flags` selects internal behaviour corresponding to the `-X` options,
including `RELIC_F_NAIVE`, `RELIC_F_NO_INDEX`, `RELIC_F_NO_MAGIC`,
`RELIC_F_NO_REORDER` and `RELIC_F_NO_STREAM`, and diagnostics a caller asks
for, such as `RELIC_F_NOTE_MIXED`, the note on a mixed-type ordering
comparison that `-vv` requests. No switch changes an answer, though two
strategies may hold different amounts while they compute one, and so one
may meet a limit the other does not.

---

# 50. Loading Programs

```c
int
relic_load_file(relic_t *,
    const char *source_name,
    FILE *);

int
relic_load_path(relic_t *,
    const char *path);

int
relic_load_string(relic_t *,
    const char *source_name,
    const char *text);
```

Loading parses rules and facts into the current program. Every loader that
takes one names the input its diagnostics point at before the input itself,
as `relic_query_prepare()` does.

Facts can also be imported from a csv or tsv table, beside the loaders:

```c
int
relic_import_file(relic_t *,
    const char *source_name,
    FILE *,
    const relic_import_t *);

int
relic_import_path(relic_t *,
    const char *path,
    const relic_import_t *);
```

`relic_import_t` names the format, the header flag, the predicate and a type
for each column. An imported fact is a fact, staged and committed as a load's
are, all or nothing. The import is specified in `specs/design-import.md`.

Multiple loads and imports append to the same logical program.

---

# 51. Validation

```c
int
relic_check(relic_t *);
```

Performs:

```text
predicate consistency
rule safety
comparison safety
dependency analysis
SCC calculation
stratification
```

Evaluation implicitly calls `relic_check()` if necessary.

---

# 52. Queries

Prepare a reusable query:

```c
relic_query_t *
relic_query_prepare(relic_t *,
    const char *source_name,
    const char *query_text);
```

`source_name` is what diagnostics about the text point at, as it is for
`relic_load_string()`, so that a caller preparing several queries can tell
their messages apart.

Destroy:

```c
void
relic_query_destroy(relic_query_t *);
```

Execute using a callback:

```c
typedef int (*relic_row_cb)(
    void *,
    const relic_value_t *,
    size_t);

int
relic_query_run(relic_t *,
    relic_query_t *,
    relic_row_cb,
    void *);
```

This avoids requiring the library to allocate an arbitrarily large result array.

A query whose plan derives each of its rows once is answered from its
rule's derivations as they are made, with no answer relation in between,
when the context sets no tuple or memory limit; specified in
`specs/design-comparison-cost.md` section 4.

---

# 53. Materialization

```c
int
relic_materialize(relic_t *);
```

computes the complete stratified fixed point.

This is equivalent to CLI `-a`.

---

# 54. Programmatic Facts

Embedding applications should be able to insert ground facts without generating source text.

Example API:

```c
int
relic_fact_add(relic_t *,
    const char *predicate,
    size_t arity,
    const relic_value_t *);
```

A fact added this way is committed to its predicate's base store, exactly
as a loaded fact is (`specs/design-facts.md` section 4): it is there when
the call returns, and a tuple or memory limit it does not fit refuses it
with nothing added. A fact the base store lacks changes the program, so
the commit discards everything derived in v1.

The next query may rematerialize as necessary.

True incremental maintenance is explicitly deferred.

---

# 55. Result Inspection

The callback is the preferred streaming interface.

A convenience result collector may also exist:

```c
relic_result_t *
relic_query_collect(...);
```

but should be implemented on top of the streaming mechanism.

---

# 56. Diagnostics

Library callers must not be forced to accept stderr output.

The context supports a diagnostic callback:

```c
enum relic_diag_level {
        RELIC_NOTE,
        RELIC_WARNING,
        RELIC_ERROR
};

typedef void (*relic_diag_cb)(
    void *,
    enum relic_diag_level,
    const relic_location_t *,
    const char *);
```

The CLI installs a diagnostic callback using `warnx()`-style formatting.

---

# 57. Library Error Handling

Public functions return:

```text
0       success
-1      failure
```

Detailed errors are available through diagnostics and optionally:

```c
const char *
relic_error(relic_t *);
```

No `errno` overloading for semantic errors.

System-call failures should preserve meaningful `errno` where useful.

---

# 58. Relational Planner

Each body atom becomes a logical scan.

Given currently bound variables, the planner determines an access pattern.

Example:

```prolog
edge(X, Y)
```

may become:

```text
SCAN edge/2
```

or, when `X` is bound:

```text
LOOKUP edge/2 key={0}
```

or when both arguments are bound:

```text
MEMBER edge/2 key={0,1}
```

Negation becomes:

```text
ANTI_MEMBER
```

Comparisons become filters.

---

# 59. Plan Representation

Plans should use a compact internal operation representation rather than a VM bytecode initially.

Example:

```c
enum relic_op_kind {
        RELIC_OP_SCAN,
        RELIC_OP_LOOKUP,
        RELIC_OP_MEMBER,
        RELIC_OP_ANTI_MEMBER,
        RELIC_OP_FILTER,
        RELIC_OP_EMIT
};
```

A rule plan may simply be an ordered array of these operations.

This keeps the evaluator direct and debuggable.

A bytecode engine is not justified for v1.

The operations, the binding decisions a plan records and the lifetime of a
plan are specified in `specs/design-plan.md`.

---

# 60. Explain Mode

`relic -e` should expose enough internal behavior to make optimization understandable.

Example:

```text
query: path(a, X)

adornment:
    path/2 bf

magic seed:
    magic_path_bf(a)

stratum 0:
    edge/2

stratum 1, SCC 0:
    path_bf/2

plan:
    magic_path_bf(X)
    lookup edge/2 [0=X]
    emit path_bf(X,Y)

recursive:
    delta path_bf/2
    lookup edge/2 [0=Y]
```

This is useful both for debugging and as an educational feature.

The library interface that delivers these lines, when they are produced, and
their exact text are specified in `specs/design-planner.md` section 8.

---

# 61. Determinism
Semantic results are sets and therefore unordered. The engine nevertheless
produces deterministic plans and a deterministic derivation order for identical
input.

Specifically:

* value identifiers, within each type, and predicate identifiers are
  assigned in order of first appearance;
* relations iterate in tuple insertion order;
* the planner breaks cost ties by source position;
* generated Magic predicates are named from the predicate and its adornment,
  never from a counter;
* no logical result and no `-e` output depends on a pointer value.

Two runs over the same input therefore produce byte-identical `-e` output and
byte-identical unsorted results. This is a testability property, not a
convenience: the golden test suite depends on it.

The hash seed is not among these properties. Every hash table answers which
identifier a key has and is never walked in slot order, so the seed decides
where a key sits and how many probes find it and nothing that any interface
reports. It is therefore drawn at random once per context, which is what
stops a program author from crafting an input that drives a table into one
probe cluster; `specs/design-hash-seed.md` specifies the seed, its source
and why the promise above survives it.

`-s` sorts rows lexicographically by column, for scripts that must not depend
on that guarantee. Because a column may hold more than one type, sorting uses a
total order: values are ordered first by type rank, integer then symbol then
string, and then by the ordering of section 10.1. This order exists only to
stabilize output and is not visible to the language.

---

# 62. Performance Targets

The implementation should comfortably support:

```text
100,000–1,000,000 EDB facts
hundreds of predicates
hundreds of rules
recursive graph workloads
```

without pathological overhead.

For selective graph queries, Magic Sets should avoid global transitive-closure materialization where possible.

No specific benchmark number is a v1 correctness requirement.

Architecture must make performance measurable and improvable.

How relic is measured against other Datalog and tabled-Prolog engines --
the engines, the workloads, how their answers are checked and how the times
are recorded -- is specified in `specs/design-bench.md`.

---

# 63. Resource Limits
The library configuration permits limits on:

```text
maximum tuples
maximum memory for relation and index storage and for the working state
    of an evaluation or a query's rewrite
maximum generated Magic predicates
maximum generated Magic rules
```

and enforces two structural limits that are not configurable:

```text
maximum predicate arity              32
maximum distinct variables per rule  64
```

Limits protect command-line and embedded users from accidental combinatorial
explosion. The command-line tool exposes the configurable limits through
repeatable `-l key=value` options; see section 40.

An exceeded limit is an ordinary error, not an abort. The diagnostic names the
limit, its configured value and the operation that exceeded it:

```text
relic: limit exceeded: tuples=1000000 while deriving path/2
```

Partial results are discarded: a query that exceeds a limit produces no rows
and a nonzero exit status, because a truncated answer set would be
indistinguishable from a complete one.

The two Magic limits are the exception when both are left unset: the rewrite
then has budgets proportional to the program, on the predicates it generates
and the literals of the rules it drafts, which bound the rules it keeps as
well, and a rewrite outgrowing one answers the query from the whole program
rather than failing it, per section 38. Setting either limit lifts every
budget.

---

# 64. Source Layout
```text
configure.ac                autoconf input
Makefile.am                 the single, non-recursive automake input
autogen.sh                  maintainer bootstrap: autoreconf -fi
librelic.pc.in              pkg-config template
m4/                         local autoconf macros
build-aux/                  generated auxiliary scripts

include/
    relic.h                 the public header, and the only installed one

lib/                        librelic
    internal.h              types and prototypes shared across lib/
    ast.h                   parser output types
    arena.c                 region allocator
    diag.c                  diagnostics, source locations, error buffer
    lex.c                   lexer
    parse.c                 recursive-descent parser
    import.c                csv and tsv table reader and importer
    symbol.c                byte-string interning
    value.c                 value table and domain identifiers
    pred.c                  predicate table keyed by (name, arity)
    rule.c                  rule compilation, variable numbering, safety
    relation.c              tuple store, canonical full-tuple index, statistics
    index.c                 secondary indexes
    depgraph.c              predicate dependency graph and SCCs
    stratify.c              stratification
    plan.c                  planner and plan representation
    explain.c               plans rendered as text for -e
    magic.c                 adornment, SIPS, Magic Sets rewriting
    eval.c                  naive and semi-naive execution
    api.c                   public entry points

cmd/relic/                  the command-line tool
    cmd.h
    import.c                the -I operand
    main.c
    output.c
    repl.c

compat/                     see section 46
man/                        relic.1, relic.3
examples/                   embed.c, a minimal librelic consumer
tests/                      see section 66
bench/                      generated workloads and timing harness
contrib/                    packaging skeletons for ports and package systems
```

A smaller initial implementation may combine closely related modules. Do not
split files merely to create abstraction.

The tool lives in `cmd/relic/` rather than the BSD-style `usr.bin/relic/`
because the whole tree is built from one top-level `Makefile.am`.

---

# 65. Build
## 65.1 Requirements

Building from a release tarball requires only:

```text
a C11 compiler
make
a POSIX shell
```

No autotools, no Python, no perl, no CMake, no Meson and no network access. The
tarball produced by `make dist` contains the generated `configure` and
`Makefile.in`.

Regenerating the build system from a source checkout requires maintainer tools:

```text
autoconf   >= 2.69
automake   >= 1.13
libtool    >= 2.4.2
```

invoked through:

```sh
./autogen.sh
```

## 65.2 Canonical build

```sh
./configure
make
make check
make install
```

Out-of-tree builds are supported and preferred:

```sh
mkdir build && cd build
../configure
make
```

`make distcheck` must pass; it runs in continuous integration at every
milestone.

## 65.3 configure options

```text
--enable-debug          assertions, -O0 -g3                    (default: no)
--enable-werror         treat warnings as errors               (default: no)
--disable-threads       evaluate on the calling thread alone   (default: threads)
--enable-asan           AddressSanitizer                       (default: no)
--enable-ubsan          UndefinedBehaviorSanitizer             (default: no)
--enable-msan           MemorySanitizer, clang on Linux        (default: no)
--enable-tsan           ThreadSanitizer                        (default: no)
--enable-coverage       gcov or llvm-cov instrumentation       (default: no)
--enable-fuzzers        build tests/fuzz, clang only           (default: no)
--with-libedit=auto|yes|no
                        line editing and history in the REPL   (default: auto)
```

Threads are POSIX threads and C11 atomics, system interfaces in the sense of
section 74. A platform lacking either builds without threads and says so;
`--enable-threads` makes their absence an error instead.

Standard autotools options apply unchanged, including `--prefix`,
`--program-prefix`, `--disable-shared`, `--disable-static` and `DESTDIR` staged
installation.

`libedit` is strictly optional. Without it the REPL reads standard input
normally and offers no history.

## 65.4 Sanitizer and coverage targets

```text
make check-asan     reconfigure with --enable-asan, rebuild, run the suite
make check-lsan     LeakSanitizer alone where it exists standalone
make check-msan     MemorySanitizer where it is supported
make check-ubsan    UndefinedBehaviorSanitizer
make check-tsan     ThreadSanitizer, in a build with threads
make check-random   the randomized differential suite, seed count configurable
make coverage       build instrumented, run the suite, write a report
```

`check-asan` exports `ASAN_OPTIONS` explicitly, because ASan defaults to
`halt_on_error=0` and `detect_leaks=0` on macOS:

```text
halt_on_error=1:detect_leaks=1:detect_stack_use_after_return=1
check_initialization_order=1:strict_string_checks=1
detect_invalid_pointer_pairs=2:abort_on_error=1
```

A target that is unsupported on the current platform is a no-op that says so,
not a failure.

## 65.5 Installed artifacts

With the default `--prefix=/usr/local`:

```text
/usr/local/bin/relic
/usr/local/lib/librelic.a
/usr/local/lib/librelic.so            platform-dependent suffix
/usr/local/lib/pkgconfig/librelic.pc
/usr/local/include/relic.h
/usr/local/share/man/man1/relic.1
/usr/local/share/man/man3/relic.3
```

Every path derives from an autoconf directory variable, so packagers relocate
the whole install with `--prefix`, `--libdir`, `--mandir` and `DESTDIR`.

## 65.6 Shared library versioning

`librelic` is built by libtool with an explicit interface version:

```text
-version-info CURRENT:REVISION:AGE
```

maintained in `configure.ac` independently of the release version. Symbols not
matching `^relic_` are not exported, and the library is compiled with hidden
visibility where the compiler supports it.

## 65.7 Makefile structure

The project uses a single, non-recursive `Makefile.am`. The tree is small,
non-recursive automake builds are faster, and one Makefile makes the
dependencies between the library, the tool and the tests correct by
construction rather than by convention.

## 65.8 Relationship to BSD make

A hand-written BSD `Makefile` is not maintained in parallel. The autotools
build is the single source of truth, so that FreeBSD ports, OpenBSD ports,
pkgsrc, Homebrew and Linux distributions consume one tarball with one configure
interface. Port and package skeletons live in `contrib/` rather than forming a
second build system.

---

# 66. Testing Strategy
## 66.1 The oracle

A naive fixed-point evaluator lives in `lib/eval.c` and is selected with
`-X naive`, or with the equivalent library configuration flag. It is roughly
150 lines and remains the differential oracle for the life of the project.

Keeping the oracle inside the library rather than reimplementing it in the test
suite means both evaluators share the parser, the program model and the
relation store, so a differential failure isolates the evaluator instead of
implicating the whole pipeline. The cost is a small amount of production code
that exists for testing; it is documented as such and is covered by the same
review and sanitizer requirements as everything else.

## 66.2 Equivalences under test

```text
semi-naive(P)          ==  naive(P)
magic(Q, P)            ==  Q(full_materialize(P))
indexed(P)             ==  no-index(P)
reordered(P)           ==  textual-order(P)
```

Each is checked over the curated corpus and over randomly generated safe
programs. Every equivalence is reachable from the command line through `-X`,
so the harness needs no private entry point into the library.

## 66.3 Harness

Tests run under the automake parallel test harness:

```text
tests/unit/     one C binary per module, plain assertions
tests/lang/     *.dl golden tests; a LOG_COMPILER runs relic and compares
                stdout, stderr and exit status against *.expected,
                *.expected-err and *.exit
tests/equiv/    differential harness across -X strategies
tests/random/   seeded program generator, differential driver and shrinker
tests/fuzz/     libFuzzer entry points, built with --enable-fuzzers, and a
                replay driver that runs their corpus in every build
bench/          generated workloads and a timing harness
```

`make check` runs the unit, language and equivalence suites, the random
driver's fault-injection self-test and a small fixed set of seeds, and replays
the fuzz corpus through both fuzz targets. `make check-random` runs the
generator over a configurable number of seeds, a thousand by default, and
runs nightly rather than per commit. The generator, the driver, the shrinker,
the fuzz targets, the coverage report and the benchmarks are specified in
`specs/design-hardening.md`.

## 66.4 Regression categories

Core regression categories:

* parsing;
* malformed syntax;
* variable safety;
* duplicate facts;
* recursion;
* mutual recursion;
* cyclic graphs;
* multiple recursive atoms;
* SCC ordering;
* stratified negation;
* rejected negative recursion;
* comparisons, including mixed-type columns;
* every query binding pattern;
* Magic adornment;
* Magic recursion;
* query and full-materialization equivalence;
* output formats;
* resource limits;
* diagnostics, checked by golden stderr comparison.

---

# 67. Fuzzing

Three libFuzzer entry points drive the public interface. The first loads and
checks its input as program text, covering the lexer, the parser, the program
model and the static analyses. The second goes on to answer every query the
input holds under the optimized and the naive, fully materializing
configurations, and fails if they disagree. The third imports its input as a
csv or tsv table, under a format and columns its first byte chooses, and fails
if an import that fails reports no error.

Fuzzing targets verify:

```text
no crashes
no hangs
no invalid memory accesses
bounded parser behavior
optimized and naive answers agree
```

Every target also links against a replay driver, so the corpora are run by
every `make check` on every compiler, including those without libFuzzer.
Their inputs, bounds and corpora are specified in `specs/design-hardening.md`.

---

# 68. Development Phases
Development is organized as fifteen stages, S0 through S14, specified in
`relic-plan.md`. The phases of Draft v1 map onto them as follows:

```text
Draft v1 phase                      stages
------------------------------------------------------
(new) build and project skeleton    S0
(new) core runtime primitives       S1
1  positive Datalog kernel          S2, S3, S4, S5
2  semi-naive execution             S6, S7
3  stratified negation              S8, with its analysis in S6
4  relational optimization          S9
5  Magic Sets                       S10
6  library stabilization            S11, S12
(new) differential and fuzz         S13
(new) documentation and release     S14
```

Milestones:

```text
M0  after S0    configure, make, make check and make distcheck all succeed
M1  after S5    positive recursive Datalog answers queries end to end
M2  after S7    semi-naive evaluation agrees with the naive oracle
M3  after S9    indexed, reordered joins; -e explains the chosen plan
M4  after S10   Magic Sets agree with full materialization
M5  after S14   relic 1.0
```

Every stage lands with its own tests. No stage is complete while `make check`
is red.

---

# 69. Post-v1 Candidate Features

Potential additions, in approximate priority order:

```text
arithmetic expressions
aggregates
relation export
incremental insertion maintenance
DRed-style deletion maintenance
parallel SCC evaluation
persistent compiled rule plans
more sophisticated cardinality estimation
additional Magic/negation optimization
```

The language comes first: arithmetic and aggregates decide which programs
can be written at all, and relation export is the counterpart of the CSV
and TSV import of `specs/design-import.md`, a small step past what a query
written with `-F csv` or `-F tsv` already produces. Incremental
maintenance and parallel evaluation change the engine rather than the
language, cost the most to build, and follow.

None should require changing basic language semantics.

Parallel SCC evaluation -- the worker pool a context owns, the round that
reads a frozen store and merges its results in the serial order, and the
measurement each step is gated by -- is specified in
`specs/design-parallel.md`.

---

# 70. Explicitly Deferred Incremental Evaluation

Version 1 uses semi-naive evaluation inside one evaluation cycle.

It does not promise incremental maintenance after arbitrary database changes.

If:

```c
relic_fact_add(...)
```

is called after materialization, the fact is committed to its predicate's
base store and, when the store lacked it, everything derived is discarded,
to be recomputed from the base stores by the next evaluation.

The internal delta-oriented architecture should nevertheless make future incremental insertion support natural: a new base tuple is exactly a delta of an extensional predicate.

Deletion support is deferred because it requires derivation tracking, DRed, or related machinery.

---

# 71. Design Principles

The following principles should guide implementation decisions.

### Relations, not proof machinery

Represent:

```text
predicate → relation → tuples
```

rather than building a miniature Prolog runtime.

### Transform before complicating the evaluator

Magic Sets should rewrite rules.

Semi-naive compilation should generate rule variants.

The execution kernel should remain simple.

### No feature without a clear Datalog use

Do not add general language features merely because Prolog has them.

### Query order is not semantics

The planner may freely reorder safe positive relational literals.

### Prefer inspectability

`-e` should make transformations visible.

Internal representations should be easy to dump.

### Prefer BSD/libc facilities

Do not introduce a dependency to save fifty lines of straightforward C.

### Optimize architecture before micro-optimization

Indexes, delta evaluation, query specialization, and join ordering matter far more than clever instruction dispatch.

---

# 72. Success Criteria
Version 1 is complete when all of the following work correctly:

```prolog
edge(a, b).
edge(b, c).
edge(c, d).

path(X, Y) :-
    edge(X, Y).

path(X, Z) :-
    edge(X, Y),
    path(Y, Z).

blocked(X) :-
    todo(X),
    depends(X, Y),
    not done(Y).

ready(X) :-
    todo(X),
    not blocked(X).
```

and the implementation can:

1. validate the program;
2. reject unsafe or unstratified programs, with a diagnostic naming the rules
   that form the cycle;
3. compute recursion using semi-naive evaluation;
4. answer `path(a, X)?` using a Magic-specialized program;
5. answer the same query by full materialization;
6. produce identical results in both modes, and identical results with and
   without indexes and body reordering;
7. expose the chosen plan with `-e`;
8. stream results through `librelic` without materializing a result array;
9. operate without third-party runtime dependencies;
10. build, install and pass its own test suite from a `make dist` tarball, with
    no autotools present, on FreeBSD, OpenBSD, NetBSD, macOS and Linux with
    both glibc and musl;
11. run the full suite clean under AddressSanitizer and
    UndefinedBehaviorSanitizer.

---

# 73. Architectural North Star

The desired conceptual implementation is:

```text
                 SOURCE
                   |
                   v
             +-----------+
             |  parser   |
             +-----------+
                   |
                   v
             +-----------+
             | relations |
             | + rules   |
             +-----------+
                   |
          +--------+--------+
          |                 |
          v                 v
   stratification       query bindings
          |                 |
          |                 v
          |           +------------+
          |           | Magic Sets |
          |           +------------+
          |                 |
          +--------+--------+
                   |
                   v
             +-----------+
             | planner   |
             +-----------+
                   |
                   v
             +-----------+
             | semi-naive|
             | evaluator |
             +-----------+
                   |
                   v
               RELATIONS
                   |
                   v
                RESULTS
```

There should be no hidden second logic engine.

Magic Sets specialize the same rule system.

Semi-naive evaluation executes the same relational plans.

Negation uses ordinary indexed absence checks after stratification.

The library and CLI use the same engine.

That simplicity is the central feature of `relic`.

---

# 74. Dependency Policy

relic has no third-party runtime dependencies. The engine and the tool use the
C11 standard library and the BSD interfaces of section 45, and nothing else.
This is a product requirement, not an aesthetic preference: the tool is meant
to be as easy to install and audit as `awk` or `grep`, and section 2.2 treats
code size as a design constraint.

The policy, in order of preference:

1. **Use the system interface.** If libc or a BSD interface does it, call it.
2. **Compile a compatibility object.** If a platform lacks that interface,
   `configure` selects a replacement from `compat/` for that platform only.
3. **Vendor a pinned upstream release.** If some future component genuinely
   needs external code, vendor a specific released version under `vendor/`,
   keeping its license, provenance and checksum, keeping the integration layer
   separate from upstream sources, documenting every patch, and building it
   under the same warnings and sanitizers as the rest of the tree. Nothing in
   v1 is expected to require this.
4. **Write it.** A purpose-built open-addressing hash table for interning and
   for relations is a few hundred lines, is faster than a generic one because
   it stores 32-bit identifiers inline, and is auditable. Reimplementing a
   general-purpose container library would not be.

The one optional dependency is `libedit`, used only for line editing and
history in the interactive mode. It is detected by `configure`, it is never
required, and its absence changes only the REPL's input handling.

Build-time and maintainer-time tools are a separate question from runtime
dependencies. autoconf, automake and libtool are maintainer tools; a release
tarball builds without them. No parser generator is used at any time, so no
generated source is committed and the grammar lives where its diagnostics do.

---

# 75. Repository and Documentation Conventions

```text
specs/architecture.md    this document: the target design of the system
specs/style.md           code style, binding on all relic-owned code
specs/design-*.md        one per subsystem, written before it is implemented
relic-plan.md            the staged implementation plan
STATUS.md                what exists today; a snapshot, not a roadmap
TODO.md                  forward-looking index of open work, by severity
initial-spec.md          the original Draft v1, kept for reference
```

This document and the `specs/design-*.md` documents are forward-looking. They
describe the system being built, not the subset that has shipped. They change
when a decision changes, not when a stage lands. What shipped belongs in
`STATUS.md`, what remains belongs in `TODO.md`, and sequencing belongs in
`relic-plan.md`. Trimming a design document to match a partial implementation
would erode the record of intent and force a later reader to reconstruct it
from commit history.

Before a subsystem is implemented it gets a `specs/design-<subsystem>.md` and a
reference from this document. The stage descriptions in `relic-plan.md` name
which design documents each stage owes.

---

# 76. Amendment Log

Changes relative to `initial-spec.md`, Draft v1.

**Naming.** The tool, library, header and symbol prefix are `relic`,
`librelic`, `<relic.h>` and `relic_` / `RELIC_`. Note that an unrelated
cryptographic toolkit also publishes a `librelic` and a `relic.h`; relic
installs its header as `$(includedir)/relic.h` rather than in a subdirectory,
and the pkg-config module is `librelic`. If a distribution reports a conflict,
the remedies are `--program-prefix` and a renamed pkg-config module, neither of
which affects the source tree.

**Build system, section 65.** GNU autoconf, automake and libtool replace the
BSD make build. A parallel hand-written BSD Makefile is not maintained.
Sanitizer, coverage and randomized-test targets are part of the build
interface. Section 64 is adjusted for a single non-recursive `Makefile.am`,
and the tool moves from `usr.bin/relic/` to `cmd/relic/`.

**Portability, section 46.** Feature availability is decided by `configure`
and `AC_REPLACE_FUNCS`. `queue.h` and `tree.h` are bundled unconditionally
instead of probed, each under its own upstream license.

**Command line, section 40.** Added a repeatable `-l key=value` for resource
limits, so new limits never consume option letters, and a repeatable
`-X behaviour` for internal behaviour, so the differential suite drives every
evaluation strategy through the ordinary interface. Options are documented in
alphabetical order.

**Value representation, section 19.** The internal representation is specified:
all values, integers included, are interned into one table and tuples are dense
arrays of 32-bit domain identifiers. The public ABI is unchanged. The
alternatives and their trade-offs are recorded in `relic-plan.md` decision D-3.

**Types.** Every struct has a matching `_t` typedef and the typedef is used in
declarations, per `specs/style.md`.

**Variables, section 7.** An identifier beginning with `_` is a variable;
`_` alone is anonymous; a longer `_`-prefixed name is an ordinary variable
exempt from the new singleton-variable warning.

**Comparisons, section 10.** Type rules are specified: cross-type equality is
false rather than an error, cross-type ordering filters the binding out,
symbols and strings order bytewise, and `=` does not bind a variable.

**Determinism, section 61.** Hash seeds are fixed at compile time. The total
order used by `-s` across mixed-type columns is specified.

**Relation representation, section 20.** The tuple store is append-only within
an evaluation, which is what lets semi-naive evaluation represent a delta as a
range of tuple identifiers.

**Execution bindings, section 25.** The 64-variable limit is a hard limit
rather than an option to fall back to a dynamic bitmap.

**Memory, section 47.** The arena failure contract is specified: `NULL` plus a
sticky flag, checked at phase boundaries, never an abort. The storage
primitives are detailed in `specs/design-memory.md`.

**Statistics, section 48.** `relic_stats_t` joins the complete public types,
filled by `relic_stats()`, so that `-X stats` and `.stats` reach the engine's
counters through the public header rather than from inside the library.

**Opaque types, section 48.** `relic_location_t` and `relic_config_t` join
`relic_value_t` as complete public types. A location is a (file, line,
column) triple that every diagnostic callback must read, and a configuration
is a plain record of limits that section 49 already specifies field by field;
making either opaque would add three accessors and five setters carrying no
information the struct does not.

**Limits, section 63.** Limits are enumerated, exposed through `-l`, and
specified to discard partial results.

**Testing, section 66.** The naive oracle lives in the library behind `-X naive`
rather than being reimplemented in the test suite. The harness and the
equivalences under test are specified.

**Development phases, section 68.** Replaced by a pointer to the fifteen stages
and six milestones of `relic-plan.md`.

**Success criteria, section 72.** Added index and reordering equivalence, a
diagnostic quality requirement for the unstratified case, a tarball-build
requirement, and a sanitizer-clean requirement.

**Queries, section 11.** The synthetic query rule is named `$q0` rather than
`__query`, per `relic-plan.md` decision D-12: `$` cannot occur in an
identifier, so the name cannot be mistaken for a user predicate. The rule's
head names no predicate; the query object owns the relation it is answered
into, per decision D-17 and `specs/design-query.md`.

**Plan representation, sections 58 and 59.** The operation set is the six of
decision D-8. `MEMBER` answers a literal whose every argument is bound through
the canonical full-tuple index, and negation is `ANTI_MEMBER`, the same test
inverted, rather than a lookup.

**Queries, section 52.** `relic_query_prepare()` takes a source name, as
`relic_load_string()` does. Query text comes from no file, and without a name
the diagnostics of several queries would all point at the same place.

**Negation, section 9.** The unstratified example gains a positive literal
in each rule. As first written, `X` occurred only under `not`, which section
14 makes unsafe, so the example was rejected for safety and never reached the
stratification check it was meant to illustrate.

**Stratification, section 30.** Each note of the unstratified diagnostic
points at the body literal that creates its edge rather than at the start of
the rule, so that a reader is shown the literal to change.

**Rule planning, section 23.** A component's rules are planned when the
component starts, rather than every rule before evaluation begins, so that the
planner measures the complete relations of the components before it; the
members of the component being planned are estimated by documented constants.
`specs/design-planner.md` section 2 records the trade-off.

**Explain mode, section 60.** `-e` output reaches the tool through
`relic_explain_set()`, a callback receiving one line of printable ASCII at a
time as each plan is built, and the tool writes it to standard error, so that
standard output carries only query results in every format.

**Magic Sets, sections 34, 37, 39 and 60.** Generated predicates are named
`path$bf` and `magic$path$bf`, per D-12 of the plan, rather than the
`path_bf` and `magic_path_bf` of the examples, which a program could collide
with. A query reads a predicate no rule derives from its base store, and
negated relations and everything a completely read relation depends on in
full; those are materialized before the rewrite, so that the rewrite's
ordering measures them.
A query against a program whose relations already hold its fixed point is not
rewritten: it reads them directly. `specs/design-magic.md` records the
reasoning.

**Magic limits, sections 38, 49 and 63.** With both left unset, the limits on
generated Magic predicates and rules are replaced by budgets proportional to
the program, one on generated predicates and one on the literals the rewrite
drafts, which bounds the rules it keeps as well, rather than being unlimited,
and a rewrite outgrowing one is abandoned with a warning and the query
answered from the whole program; setting either lifts them all.
Adornments are finite but can be exponential in the arity, and an optimization must neither decide whether a
query is answered nor generate a program unboundedly larger than the one it
specializes.

**Determinism, section 61.** The hash seed is drawn at random once per
context rather than fixed at compile time. No result, `-e` line, identifier,
capacity or memory charge depends on it, so the byte-identical promise stands
on the remaining properties, and a program author can no longer craft an
input that drives a table into one probe cluster. `getentropy(2)` joins the
BSD interfaces of section 45 and the portability layer of section 46.
Specified in `specs/design-hash-seed.md`.

**Source layout, section 64.** `lib/explain.c` renders plans as text, and
relation statistics live with the tuple store in `lib/relation.c`.

**Table import, sections 40, 41, 48, 50, 64, 67 and 69.** Facts can be
imported from csv and tsv tables: `-I` on the command line, which loads after
the program's files, and `relic_import_file()` and `relic_import_path()`
through the library, described by `relic_import_t`, a fifth complete type.
The reader and importer are `lib/import.c`, and a third fuzz target imports
its input as a table. Specified in `specs/design-import.md`.

**Comparison cost, sections 19, 22, 33, 43, 47 and 52.** The engine
comparison found relic trailing on the cost of delivering an answer and
of loading facts rather than on evaluation. The tool formats rows into a
buffer of its own; a query whose head keeps every body variable streams
its rule's derivations with no answer relation; a free adornment subsumes
the bound adornments of its predicate and a rewrite with no bound column
reads the program's relations; a secondary index stores each key's values
and its identifier table carries a tag byte per slot; a domain identifier
carries its type, so the value table holds integers only and a symbol's
bytes are packed; and arrays past a page threshold are mapped so a growth
leaves no residue. D-3's decision stands with a new encoding. Specified in
`specs/design-comparison-cost.md`.

**New sections.** 74 dependency policy, 75 repository and documentation
conventions, 76 this log.
