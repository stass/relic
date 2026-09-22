# dlog — Small General-Purpose Datalog for C

**Status:** Draft v1 Engineering Specification
**Working name:** `dlog`
**License target:** 2-clause BSD
**Primary implementation language:** ISO C11
**Primary platforms:** FreeBSD, OpenBSD, NetBSD, macOS
**Secondary platform:** Linux via a small compatibility layer

---

# 1. Summary

`dlog` is a small, dependency-free Datalog implementation intended to occupy the space between tiny educational interpreters and large research/database systems.

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
dlog graph.dl
```

or:

```sh
dlog -q 'path(a, X)?' graph.dl
```

The implementation should remain comprehensible enough that a systems programmer can understand the evaluator without studying a full logic-programming runtime.

---

# 2. Product Goals

## 2.1 Primary goals

`dlog` shall:

1. Implement a useful, conventional subset of Datalog with well-defined semantics.
2. Correctly evaluate recursive rules using semi-naive bottom-up evaluation.
3. Support safe stratified negation.
4. Use Magic Sets to specialize evaluation for bound queries.
5. Represent predicates internally as relational tables rather than Prolog-like clauses.
6. Support efficient joins using indexes and basic join ordering.
7. Operate as both:

   * a command-line program; and
   * a reusable C library.
8. Require no external runtime, VM, parser generator, database, or third-party library.
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
* parallel execution;
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

Variables begin with an uppercase ASCII letter:

```prolog
X
Node
Dependency
```

`_` is an anonymous variable.

Every occurrence of `_` denotes a distinct variable.

Example:

```prolog
edge(X, _).
```

Variable scope is one rule or query.

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
a(X) :- not b(X).
b(X) :- not a(X).
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

Ordering is valid only between values of the same type.

Version 1 comparisons are filters.

They do not generate arbitrary new values.

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

Internally, every query is transformed into a synthetic relation:

```prolog
__query(X, Y) :-
    depends(X, Y),
    status(Y, todo).
```

The query output columns are the named variables in first-occurrence order.

Anonymous variables are omitted.

Queries have set semantics.

Result order is unspecified unless sorting is explicitly requested.

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
struct dlog_pred {
        uint32_t        name;
        uint16_t        arity;
        ...
};
```

Predicate names are interned.

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

The public ABI should use an explicit representation:

```c
enum dlog_type {
        DLOG_SYMBOL,
        DLOG_STRING,
        DLOG_INTEGER
};

struct dlog_value {
        enum dlog_type type;
        union {
                uint32_t id;
                int64_t  integer;
        } v;
};
```

The internal representation may later use compact tagged 64-bit values provided this remains invisible to library consumers.

---

# 20. Relation Representation

Every predicate owns a relation.

Conceptually:

```c
struct dlog_relation {
        struct dlog_pred        *pred;
        struct dlog_tuple_store tuples;
        struct dlog_index_head  indexes;
};
```

Tuples should be stored densely.

Indexes should refer to tuple IDs rather than duplicate complete tuples.

Example:

```text
depends/2

tuple 0: parser lexer
tuple 1: planner parser
tuple 2: eval planner
```

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
bool
dlog_relation_insert(...);
```

returns true only when the tuple was newly inserted.

This property drives fixed-point detection.

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

---

# 25. Execution Bindings

Rule evaluation operates on variable bindings.

Each compiled rule assigns small integer IDs to variables:

```text
X → 0
Y → 1
Z → 2
```

An evaluation frame may therefore resemble:

```c
struct dlog_binding {
        struct dlog_value *values;
        uint64_t           bound;
};
```

For rules with more than 64 variables, a fallback dynamic bitmap may be used, or v1 may reject such rules.

A 64-variable rule limit is acceptable for v1.

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

---

# 29. SCC Evaluation

Construct the positive predicate dependency graph.

Compute strongly connected components.

Rules in acyclic SCCs require no fixed-point loop.

Recursive SCCs are evaluated using semi-naive deltas.

SCCs within each stratum are processed in topological order.

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
policy.dl:18:1: note: ready/1 depends negatively on blocked/1
policy.dl:24:1: note: blocked/1 depends on ready/1
```

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

`dlog` uses the same greedy ordering algorithm used by the relational planner.

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

For realistic Datalog arities this is naturally finite.

Duplicate rewritten rules must be structurally deduplicated.

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
dlog
```

No separate compiler is required for v1.

Utility syntax follows traditional BSD command-line conventions.

No GNU-style mandatory long options are required.

Usage:

```text
dlog [-aceinsvV] [-F format] [-o file] [-q query] [file ...]
```

Options:

```text
-a        materialize the complete program; do not specialize queries
-c        check syntax, safety, and stratification; do not evaluate
-e        print the selected execution plan / Magic rewrite
-i        enter interactive mode after loading input
-n        suppress normal query output; useful with -e or -v
-q expr   execute expr as a query; may be specified multiple times
-s        sort query results before printing
-v        increase diagnostic verbosity; repeatable
-V        print version and exit

-F fmt    output format: text, csv, tsv, json
-o file   write query output to file instead of stdout
```

Files are operands rather than arguments to a special file option.

This is preferred:

```sh
dlog rules.dl facts.dl
```

rather than:

```sh
dlog -f rules.dl
```

If no files are given and standard input is not a terminal, the program is read from stdin.

---

# 41. CLI Examples

Validate:

```sh
dlog -c project.dl
```

Run queries embedded in a file:

```sh
dlog project.dl
```

Supply an external query:

```sh
dlog -q 'ready(X)?' project.dl
```

Query CSV output:

```sh
dlog -F csv -q 'depends(X, Y)?' project.dl
```

Inspect optimization:

```sh
dlog -e -q 'path(a, X)?' graph.dl
```

Force whole-program materialization:

```sh
dlog -a graph.dl
```

Interactive use:

```sh
dlog -i graph.dl
```

Pipeline:

```sh
cat graph.dl | dlog -q 'path(a, X)?'
```

---

# 42. Interactive Mode

The REPL accepts ordinary queries directly:

```text
dlog> path(a, X)?
b
c
d

dlog> status(X, todo)?
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

The implementation should prefer BSD facilities where they improve simplicity.

Expected headers/APIs include:

```c
#include <sys/queue.h>
#include <sys/tree.h>
#include <err.h>
#include <sysexits.h>
#include <unistd.h>
```

Use:

```text
getopt(3)
err(3)
errx(3)
warn(3)
warnx(3)
reallocarray(3)
```

where appropriate.

`TAILQ`, `STAILQ`, or `SLIST` should be used for compiler/evaluator worklists when they simplify ownership.

`RB_TREE` from `<sys/tree.h>` may be used for small ordered structures where hashing is unnecessary.

Performance-critical relation and symbol lookup should use purpose-built open-addressing hash tables rather than red-black trees.

---

# 46. Portability Layer

Linux and other non-BSD systems may lack some BSD functions and headers.

A small:

```text
compat/
```

layer may provide:

```text
reallocarray
strlcpy / strlcat if actually needed
sys/queue.h
sys/tree.h
```

No broad portability framework should be introduced.

Platform conditionals should remain isolated from the evaluator.

---

# 47. Memory Management

The engine should use arenas for short-lived compiler structures.

Recommended lifetimes:

```text
context arena      symbols, predicate definitions
program arena      rules, parsed terms
query arena        Magic rewrite and query plans
evaluation arena   transient binding/join state
```

Relation tuples remain independently owned for the lifetime of the database.

Avoid one `malloc()` per AST node or tuple.

Use:

```c
reallocarray()
```

for growable arrays.

---

# 48. C Library

Public library:

```text
libdlog
```

Public header:

```text
<dlog.h>
```

All major objects are opaque.

Example:

```c
struct dlog;
struct dlog_query;
struct dlog_result;
```

Applications must not depend on internal layout.

---

# 49. Library Lifecycle

Representative API:

```c
struct dlog *
dlog_create(void);

void
dlog_destroy(struct dlog *);
```

Configuration variant:

```c
struct dlog_config {
        size_t max_memory;
        size_t max_tuples;
        unsigned flags;
};

struct dlog *
dlog_create_config(const struct dlog_config *);
```

Zero values mean default/unlimited where appropriate.

---

# 50. Loading Programs

```c
int
dlog_load_file(struct dlog *,
    FILE *,
    const char *source_name);

int
dlog_load_path(struct dlog *,
    const char *path);

int
dlog_load_string(struct dlog *,
    const char *source_name,
    const char *text);
```

Loading parses rules and facts into the current program.

Multiple loads append to the same logical program.

---

# 51. Validation

```c
int
dlog_check(struct dlog *);
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

Evaluation implicitly calls `dlog_check()` if necessary.

---

# 52. Queries

Prepare a reusable query:

```c
struct dlog_query *
dlog_query_prepare(struct dlog *,
    const char *query_text);
```

Destroy:

```c
void
dlog_query_destroy(struct dlog_query *);
```

Execute using a callback:

```c
typedef int (*dlog_row_cb)(
    void *,
    const struct dlog_value *,
    size_t);

int
dlog_query_run(struct dlog *,
    struct dlog_query *,
    dlog_row_cb,
    void *);
```

This avoids requiring the library to allocate an arbitrarily large result array.

---

# 53. Materialization

```c
int
dlog_materialize(struct dlog *);
```

computes the complete stratified fixed point.

This is equivalent to CLI `-a`.

---

# 54. Programmatic Facts

Embedding applications should be able to insert ground facts without generating source text.

Example API:

```c
int
dlog_fact_add(struct dlog *,
    const char *predicate,
    size_t arity,
    const struct dlog_value *);
```

Adding facts after evaluation invalidates derived state in v1.

The next query may rematerialize as necessary.

True incremental maintenance is explicitly deferred.

---

# 55. Result Inspection

The callback is the preferred streaming interface.

A convenience result collector may also exist:

```c
struct dlog_result *
dlog_query_collect(...);
```

but should be implemented on top of the streaming mechanism.

---

# 56. Diagnostics

Library callers must not be forced to accept stderr output.

The context supports a diagnostic callback:

```c
enum dlog_diag_level {
        DLOG_NOTE,
        DLOG_WARNING,
        DLOG_ERROR
};

typedef void (*dlog_diag_cb)(
    void *,
    enum dlog_diag_level,
    const struct dlog_location *,
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
dlog_error(struct dlog *);
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
ANTI_LOOKUP
```

Comparisons become filters.

---

# 59. Plan Representation

Plans should use a compact internal operation representation rather than a VM bytecode initially.

Example:

```c
enum dlog_op {
        DLOG_SCAN,
        DLOG_LOOKUP,
        DLOG_FILTER,
        DLOG_ANTI_LOOKUP,
        DLOG_EMIT
};
```

A rule plan may simply be an ordered array of these operations.

This keeps the evaluator direct and debuggable.

A bytecode engine is not justified for v1.

---

# 60. Explain Mode

`dlog -e` should expose enough internal behavior to make optimization understandable.

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

---

# 61. Determinism

Semantic results are sets and therefore unordered.

The engine should nevertheless produce deterministic plans for identical input.

Hash randomization must not affect logical results.

`-s` provides deterministic sorted output for tests and scripts.

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

---

# 63. Resource Limits

The library configuration should permit limits on:

```text
maximum tuples
maximum memory
maximum generated Magic predicates/rules
maximum rule variables
maximum predicate arity
```

Limits protect CLI and embedded users from accidental combinatorial explosion.

Errors should clearly identify which limit was exceeded.

---

# 64. Source Layout

Suggested tree:

```text
include/
    dlog.h

lib/
    arena.c
    diag.c
    eval.c
    index.c
    lexer.c
    magic.c
    parse.c
    plan.c
    relation.c
    rule.c
    stratify.c
    symbol.c
    value.c

usr.bin/dlog/
    main.c
    repl.c
    output.c

compat/
    ...

tests/
    ...
```

A smaller initial implementation may combine closely related modules.

Do not split files merely to create abstraction.

---

# 65. Build

Canonical BSD build should support:

```sh
make
make test
make install
```

On FreeBSD, use the normal BSD make infrastructure where practical.

The installed artifacts are:

```text
/usr/local/bin/dlog
/usr/local/lib/libdlog.a
/usr/local/lib/libdlog.so
/usr/local/include/dlog.h
/usr/local/share/man/man1/dlog.1
/usr/local/share/man/man3/dlog.3
```

A portable non-BSD Makefile may be provided without requiring CMake, Meson, autotools, or Python.

---

# 66. Testing Strategy

A tiny naive reference evaluator should exist in the test suite.

It need not be part of production code.

Production semi-naive results are compared against naive fixed-point results.

Magic execution should similarly be tested against full materialization:

```text
magic(query, program)
==
query(full_materialize(program))
```

for randomly generated safe programs.

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
* comparisons;
* every query binding pattern;
* Magic adornment;
* Magic recursion;
* query/full-materialization equivalence;
* output formats;
* resource limits.

---

# 67. Fuzzing

Lexer and parser should expose an optional libFuzzer entry point.

Fuzzing targets should verify:

```text
no crashes
no hangs
no invalid memory accesses
bounded parser behavior
```

A second future fuzz target may compare optimized and naive execution on generated programs.

---

# 68. Development Phases

## Phase 1 — Positive Datalog kernel

Implement:

```text
lexer/parser
symbols
facts
rules
safety
relations
naive fixed point
queries
CLI
```

This establishes language correctness.

## Phase 2 — Semi-naive execution

Add:

```text
dependency graph
SCCs
delta relations
recursive delta rule variants
```

Retain naive evaluator in tests.

## Phase 3 — Stratified negation

Add:

```text
negative dependency edges
stratification
anti-lookups
safety checking
```

## Phase 4 — Relational optimization

Add:

```text
secondary indexes
bound-column access paths
greedy body ordering
basic relation statistics
```

## Phase 5 — Magic Sets

Add:

```text
query synthetic predicate
binding adornments
SIPS
magic seeds
magic propagation
specialized predicates
explain output
```

Full materialization remains available for equivalence testing.

## Phase 6 — Library stabilization

Finalize:

```text
public ABI
diagnostic callbacks
streaming result API
embedding documentation
manual pages
```

---

# 69. Post-v1 Candidate Features

Potential additions, in approximate priority order:

```text
CSV/TSV fact import
arithmetic expressions
incremental insertion maintenance
DRed-style deletion maintenance
aggregates
parallel SCC evaluation
persistent compiled rule plans
relation export
more sophisticated cardinality estimation
additional Magic/negation optimization
```

None should require changing basic language semantics.

---

# 70. Explicitly Deferred Incremental Evaluation

Version 1 uses semi-naive evaluation inside one evaluation cycle.

It does not promise incremental maintenance after arbitrary database changes.

If:

```c
dlog_fact_add(...)
```

is called after materialization, affected derived state may be discarded and recomputed.

The internal delta-oriented architecture should nevertheless make future incremental insertion support natural.

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
2. reject unsafe or unstratified programs;
3. compute recursion using semi-naive evaluation;
4. answer `path(a, X)?` using a Magic-specialized program;
5. answer the same query by full materialization;
6. produce identical results in both modes;
7. expose the chosen plan with `-e`;
8. stream results through `libdlog`;
9. operate without third-party runtime dependencies;
10. build cleanly with a BSD C toolchain.

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

That simplicity is the central feature of `dlog`.
