# Architecture

relic is a small Datalog engine written in ISO C11: one command-line tool,
`relic`, and one embeddable library, `librelic` with the public header
`<relic.h>`, licensed under the 2-clause BSD license. This document is the map
of the system, read before any subsystem document: what it is for, the
principles it is built on, the pipeline from source text to answers with the
invariants of each stage, the contracts that cross every subsystem, and the
layout of the tree. Each pipeline stage links to the design document that
specifies it.

Related: [specs index](README.md), [language](language.md),
[code style](style.md), relic(1), relic(3).

## Summary

relic is meant to occupy the space between tiny educational interpreters
and large research or database systems:

> A small relational inference engine that feels like `awk`, `sqlite3`, or
> `grep`: easy to install, easy to embed, predictable, scriptable, and
> general enough for dependency analysis, policy, static analysis,
> configuration, graph queries, project tracking and other small-to-medium
> deductive workloads.

The language is conventional function-free Datalog with set semantics.
A program states facts, derives relations with rules, and asks queries; a
predicate is identified by its name and arity. Values are symbols, strings
and 64-bit integers. Rules may negate a literal and fold a group with
`count`, `sum`, `min` or `max`, both through stratification, and may
compute with integer arithmetic and byte-string functions inside
comparisons. A recursive program means its least fixed point, and a
stratified one means the fixed point of each stratum in turn. The language
is specified in [language.md](language.md); this document does not repeat
it.

```prolog
edge(a, b).
edge(b, c).
edge(c, d).

path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).

path(a, X)?
```

`relic graph.dl` answers the query the file asks; `relic -q 'path(b, X)?'
graph.dl` asks another.

The engine evaluates bottom up over relations: semi-naive evaluation for
recursion, stratified negation and aggregation, a Magic Sets rewrite that
specializes evaluation to a query's bindings, and a greedy join planner
over indexes. The implementation must stay comprehensible enough that a
systems programmer can understand the evaluator without studying a
logic-programming runtime.

The guiding architectural rule is:

> The evaluator operates on relations and tuples, not on a Prolog
> execution stack.

## Goals

relic shall:

1. Implement a useful, conventional subset of Datalog with well-defined
   semantics, including safe stratified negation and aggregation.
2. Evaluate recursion correctly with semi-naive bottom-up evaluation, over
   predicates represented as relational tables rather than clauses.
3. Specialize evaluation to bound queries with Magic Sets, and make joins
   efficient with indexes and basic join ordering.
4. Serve as a command-line program and as a reusable C library, with one
   engine behind both.
5. Require no external runtime, VM, parser generator, database or
   third-party library; the sole optional dependency is `libedit`.
6. Build naturally on BSD systems using BSD libc facilities, and portably
   elsewhere.
7. Be deterministic: the same input gives byte-identical output on every
   run, and the same answers under every evaluation strategy and at every
   thread count.

## Non-goals

relic does not implement:

* Prolog, cut, clause-order execution semantics, or backtracking as a
  visible execution model;
* structures, compound function terms, or lists as a language construct;
* dynamic predicates, arbitrary foreign-function calls or
  meta-programming;
* unstratified negation, well-founded semantics or answer-set programming;
* declarations of a relation's columns or their types;
* unsigned integers;
* subsumption: rules that delete a tuple another dominates;
* a second representation of a relation, such as a union-find store for an
  equivalence relation;
* SQL, transactions or persistent database files;
* incremental maintenance across arbitrary insertions and deletions;
* distributed execution;
* cost-based optimization comparable to an SQL database.

Most of these are outside Datalog or outside the character of a small
tool. A few are left out for reasons specific to this design:

* **Declarations** would cost a line per relation in every program, most of
  the text of a small one, for checks that `-vv` and the
  undefined-predicate warning already give. Columns stay untyped, and the
  [type rules](language.md#values-and-types) make every mixed-type column
  meaningful.
* **Unsigned integers** are a second numeric type with overflow rules of
  their own, for a use no workload has shown.
* **Subsumption** deletes tuples. The tuple store is append-only, and both
  semi-naive deltas and the deferred rounds of parallel evaluation rest on
  that (see [Storage](#storage)).
* **An equivalence relation stored as a union-find**, as Souffle's `eqrel`
  does, holds an alias analysis's quadratic closure in linear space, but it
  is a second kind of relation that every join, index, delta and merge would
  have to read, for one shape of program. relic keeps one relational engine
  (see [North star](#north-star)).
* **Incremental maintenance.** A fact added after evaluation is committed
  to its predicate's base store and, when the store lacked it, everything
  derived is discarded and re-derived by the next evaluation (see
  [Facts](#facts)). Incremental insertion fits the architecture -- a new
  base tuple is exactly a delta of an extensional predicate -- but deletion
  needs derivation tracking such as DRed, which is machinery on every path.

Floating point is excluded from the language but not declined: a float
could break cross-platform determinism and the one identifier per value
that interning rests on. [design-floats.md](design-floats.md) specifies a
float type that keeps both, and the syntax it needs is
[reserved](language.md#reserved-syntax).

## Intended uses

Dependency graphs and build dependencies, project and task tracking,
package relationships, filesystem metadata, access-control and firewall
policy, software architecture and static-analysis facts, call graphs and
reachability, configuration validation, network topology, compiler facts,
small knowledge bases, and Unix pipeline data processing.

relic should perform well when the facts number from thousands to millions
of tuples and the rules from tens to hundreds. It is not an analytical
database for billions of tuples.

## Principles

**Relations, not proof machinery.** Represent `predicate -> relation ->
tuples` rather than building a miniature Prolog runtime.

**Transform before complicating the evaluator.** Magic Sets rewrite rules;
semi-naive compilation generates rule variants; aggregates generate group
rules. The executor runs a plan -- an ordered array of operations -- and is
written once. Every optimization (indexes, body reordering, semi-naive
delta variants, existence checks, the Magic rewrite, streaming, sharing a
round among threads) changes what the planner emits or how a round's
results are stored, never what the executor means by a plan. Each can be
switched off to compare against -- through `-X`, or for [shared
rounds](#parallel-evaluation) with `-j 1` -- which is what lets each optimization arrive without
rewriting the executor and what lets the differential tests check it.
This is the decision the others rest on.

**No feature without a clear Datalog use.** Do not add a language feature
because Prolog has it.

**Query order is not semantics.** The planner may freely reorder safe
positive literals; the order a body is written in decides nothing but
diagnostics and tie-breaks.

**Prefer inspectability.** `-e` shows every plan and rewrite; internal
representations are easy to dump; diagnostics name the rule and literal
they are about.

**Prefer BSD and libc facilities.** Do not introduce a dependency to save
fifty lines of straightforward C (see [Dependency
policy](#dependency-policy)).

**Optimize architecture before micro-optimization.** Indexes, delta
evaluation, query specialization and join ordering matter far more than
clever instruction dispatch, which is why plans are interpreted and no
bytecode or code generator exists.

## North star

```text
                     source
                       |
                    parser
                       |
              relations + rules
                 /           \
       stratification     query bindings
                 \            |
                  \      Magic Sets
                   \         /
                     planner
                        |
               semi-naive evaluator
                        |
                    relations
                        |
                     results
```

There is no hidden second logic engine. Magic Sets specialize the same rule
system; aggregates are generated rules of the same system; semi-naive
evaluation executes the same relational plans; negation is an ordinary
indexed absence check after stratification; parallel evaluation runs the
same plans over a frozen store -- the store as it stood when the round
began -- and merges in the serial order. The library
and the tool use the same engine. That simplicity is the central feature of
relic.

## Pipeline

The whole flow, from text to answers:

```text
program text, csv/tsv tables, relic_fact_add()
  -> lexer and parser        statements; ground facts staged, then
                             committed to base stores all or nothing
  -> check                   predicates by (name, arity), variables
                             numbered, safety, compiled rules
  -> analysis                signed dependency graph, components,
                             strata, the evaluation schedule
  -> query                   compiled as a rule whose head names no
                             predicate; answered into the query's own
                             relation
  -> Magic rewrite           query-directed mode: rules specialized to
                             the query's bindings
  -> planner                 one plan per rule (two for some delta
                             variants), made when its component starts
  -> evaluator               naive or semi-naive fixed point, rounds
                             optionally shared among threads
  -> rows                    streamed to a callback, or gathered first
  -> tool                    text, csv, tsv or json on standard output
```

### Front end

The lexer (`lib/lex.c`) is byte-oriented and locale-independent: it classifies
bytes from its own table, never `<ctype.h>`, so whether a program parses does
not depend on the process locale. It interns nothing and reports no syntax
mistake: a malformed token comes back carrying its reason, and the lexer's only
report is a refusal to grow its string buffer, as out of memory. The parser
(`lib/parse.c`) owns every syntax diagnostic and all error recovery, so
recovery lives in one place: after the first mistake in a statement it skips to
the statement's terminator and resumes, so a statement produces at most one
message and a file with five mistakes reports five. A load is all or nothing:
statements are parsed onto the load's own list and its ground facts into
staging, and nothing joins the program until the whole input has parsed and the
facts have committed.

The check (`lib/rule.c`, `lib/pred.c`) turns statements into compiled
rules. Safety is one set computation -- the closure of the variables bound
by positive literals, assignments and aggregate results -- rather than a
search over orderings, so it needs no planner and reports offenders in
textual order by construction (see [Safety](language.md#safety)). The check
runs on demand and is cached until the program changes.

See [design-frontend.md](design-frontend.md).

### Values

Every value, integers included, is interned, and a tuple is a dense array
of 32-bit domain identifiers: four bytes per column whatever the type, so
joins, hashing and duplicate elimination are integer work that never
inspects a payload. The top two bits of an identifier give its type: a
symbol or a string is its symbol identifier under the symbol or string type
bits, and an integer is its index in the integer table under the integer
type bits. Zero is never an identifier.

Identifiers are assigned in order of first appearance within each type, so
they are reproducible, but their numeric order means nothing: **nothing may
order values by domain identifier**; ordering decodes the values. A value
evaluation computes is interned when its output batch or admitted round
is committed, before tuples naming it are inserted.

The rejected alternatives are a tagged 64-bit cell, whose boxed integers have
two encodings every construction site must keep canonical, and the public value
struct, whose padding defeats a word-wise hash. The representation sits behind
a few accessors, so it can be replaced without touching the planner, evaluator,
rewrite or public interface.

See [design-values.md](design-values.md).

### Storage

A predicate has at most two relations: its **base store**, holding the facts
the program states, and its **derived relation**, which exists once a rule
derives the predicate. A relation's tuples are an append-only dense array,
and a tuple identifier, once assigned, names the same tuple until the
program changes. Semi-naive evaluation rests on that: the tuples one round
derived, its **delta**, are a half-open range of identifiers, with no copy
and no separate relation.

Every relation has a canonical index over the whole tuple, which answers
membership. Insertion reports newness separately from failure, because a
duplicate means the fixed point is closer and a failure means the answer is
unknown, and it performs every fallible step before any mutation, because a
tuple stored but not indexed is silent corruption. Secondary indexes are
built on demand for the bound columns of a lookup and never published half
built; they list a key's tuples in identifier order, so a lookup sees them
in the order a scan would, and can start at the first tuple of a delta.

See [design-storage.md](design-storage.md).

### Facts

Facts are tuples, not program objects: a ground fact costs a tuple and an
index slot, with nothing in the rule table or schedule. A load, an import
and `relic_fact_add()` reach the base stores through one commit that
measures and reserves everything first and then adopts or appends infallibly,
so a refused commit adds nothing and needs no unwinding. A predicate is derived
exactly when a rule with it as head has been added; its derived relation
is seeded from its base store when evaluation starts, and the Magic
rewrite reads the base store on its own through the base predicate
`p$base` (see [design-magic.md](design-magic.md#facts-and-the-base-predicate)).

Private fact builders own the same tuple and canonical-index payload as
base relations. An empty destination takes ownership at commit, avoiding
a second full copy; a populated destination receives a reserved append.
Table imports prepare bounded windows with private worker dictionaries.
The coordinator preserves global first-occurrence value identifiers and
publishes a window only when monotone projections prove every scalar
prefix admissible. An unproved window replays through the scalar importer,
preserving error order and the values retained after a failed input.
Only the final load commit publishes base facts. This keeps parallel work
outside identifier assignment and leaves the domain encoding unchanged.

Whether the relations hold the program's fixed point is decided by a pair
of counts that only grow, of rules and of facts. When either has grown
since the relations were computed, everything derived is discarded and
re-derived from the base stores by the next evaluation.

See [design-facts.md](design-facts.md) and
[bulk fact construction and import](design-bulk-load.md).

### Analysis

The dependency graph is signed: a body literal makes a positive, a negative or
an aggregate edge from the rule's head to the literal's predicate. Strongly
connected components are found by an iterative Tarjan's algorithm, so a deep
program cannot exhaust the C stack, and the order in which components complete
is already a topological order: dependencies first. A negative or aggregate
edge inside a component is the error "program is not stratified". It is
reported at the rule holding the component's first such edge in written order,
with a note at each body literal along the shortest cycle through that edge --
not necessarily the shortest cycle in the component (see
[Stratification](language.md#stratification)). The result is a schedule:
components in evaluation order, each with its rules and whether it is
recursive.

See [design-stratify.md](design-stratify.md).

### Planning

A plan is data: an ordered array of operations -- generators that scan a
relation or look it up through an index, membership checks, existence checks,
absence checks and comparisons, an assignment, an aggregate fold, and the emit
of a head tuple. Whether each column is matched or bound is decided when the
plan is built, so the executor never consults a bound mask at run time. A query
is planned as a rule.

The planner is greedy: it prefers literals with more bound arguments and
smaller estimated relations, breaks ties by source position, and places
every comparison and negated literal at the earliest point its variables
are bound. Estimates are integer arithmetic, so plans are identical on
every platform. A component's rules are planned when the component starts,
so every relation of a lower component is measured complete. A delta
variant retains at most two structural alternatives: delta-first and an
input-first order that defers the delta on equal bound-column rank.
Guessed delta cardinality does not decide whether an alternative exists.
Before any round item executes, the calling thread chooses from integer
work estimates using the same fixed views as execution and direct const
reads of existing index key counts. An alternative retains access
requirements without creating its extra indexes. Selection includes the
cost of building missing indexes and uses explicit provisional statistics
for them; all choices precede first-use preparation at the round boundary.
Only prepared plans reach the executor, and every published index remains
complete and maintained by ordinary insertion. Selection allocates nothing,
reads no tuples and depends on neither thread count nor timing. This
separates retaining a useful alternative from taxing every insertion for
an unused one, while keeping runtime adaptation outside the executor and
preserving the serial/shared-round equivalence. Ordering a body costs a
bounded number of passes over it, and the plans' memory is charged to the
memory limit, so a limit on memory is a limit on planning time.
`relic_explain_set()` installs the callback that receives the text `-e`
prints, one line of printable ASCII at a time as each plan is built (see
[Explain](design-planning.md#explain)).

See [design-planning.md](design-planning.md) and
[recursive plan selection](design-recursive-planning.md).

### Evaluation

The executor (`lib/exec.c`) runs a plan with an explicit loop and no C
recursion, and hands its derivations back in batches; it never inserts. A
recursive component -- a set of mutually recursive predicates, its
members -- is evaluated semi-naively (`lib/eval.c`): each round runs rule
variants in which one recursive literal reads the previous round's delta,
so every new derivation uses a new tuple and each is made exactly once.
What each literal may see is fixed when its plan starts, so when a batch is
inserted is invisible to the plan. The naive fixed point is kept behind
`-X naive` as the differential oracle, sharing everything but the loop.

A query whose plan derives each row exactly once, and computes and folds
nothing, streams its rows to the caller unless a tuple or memory limit is
set; otherwise its answer is gathered in a relation the query owns. Either
way no row is delivered before everything that can fail has finished, so a
failed query delivers nothing. The exception is an interrupt, which may
cut a run short after some rows, and the run's -1 says they are a prefix.
An [evaluation error](language.md#evaluation-errors) such as an integer
overflow fails the evaluation rather than dropping a binding, because a
dropped binding could make a negation true.

See [design-eval.md](design-eval.md).

### Expressions

Integer arithmetic, the string functions and regular-expression matching are
evaluated inside comparisons. The expression tree is kept, because the planner,
the executor and the explain printer each need its shape; the executor runs it
as a postfix program over typed cells. A value an expression computes lives in
the frame and is interned only when a tuple holding it is stored, through a
measure, rewrite and commit sequence with identifiers predicted from the
tables' counts, so the executor never writes to the context mid-plan. That is
what keeps the store frozen during a [shared round](#parallel-evaluation) and
leaves the tables untouched by a merge that is declined. `match` uses relic's
own matcher, a Thompson simulation over a fixed dialect, so a pattern means the
same on every platform and costs at most the text times the pattern (see
[Regular expressions](language.md#regular-expressions)).

See [design-expressions.md](design-expressions.md).

### Aggregates

An aggregate's body becomes an ordinary generated group rule, named
`agg$...`, whose head is the variables the body shares with the rule and
then its local ones, so the generated relation is the group by
construction and its set semantics are the aggregate's. A body that is one
atom of distinct variables is folded in place through an index on its
shared columns, with no generated rule. The reading rule depends on the
folded predicate through an aggregate edge, stratified like negation, so a
group is complete before it is folded. `sum` is exact in a 128-bit
accumulator, so both its value and its overflow are functions of the
group's set of rows, never of the order they arrived in.

See [design-aggregates.md](design-aggregates.md).

### Magic Sets

The rewrite (`lib/magic.c`) specializes a query's program to its bound
arguments. Predicates reached through negation or an aggregate, and what
they depend on, are read completely, evaluated from the program's own
schedule first, so the rewrite never crosses a negative edge. The rest is
read on demand through adorned copies such as `path$bf`, guarded by magic
relations such as `magic$path$bf`; generated names contain `$`, which no
program identifier can, so they cannot collide. Ordinary demand calls
follow the planner's body order.

A bound right-linear adornment whose recursive rules separate state from
unchanged outputs is factored into state propagation and base answers.
Each `walk$path$bf` tuple retains both the original requested binding and
the current state. This preserves correlation across callers and avoids
materializing answers for every intermediate state. Eligibility requires
positive relational rules over complete external inputs; the whole
adornment is certified before generation. The transformation emits ordinary
rules, uses the same rewrite budget and leaves the original program
available for complete reads and fallback. `-X no-factor` retains ordinary
Magic as an independent reference.

One limit, `max_magic`, meters everything the rewrite reads and generates;
a rewrite that outgrows it is abandoned with a warning and the query
answered from the whole program (see [Query modes](#query-modes)).

See [design-magic.md](design-magic.md) and
[bound query factoring](design-query-factoring.md).

### Parallel evaluation

A context may own a pool of worker threads. A round that reads what it
writes only through views -- ranges of tuple identifiers fixed before it
began -- can be a **deferred round**, one whose insertions wait until its
end, so its plans read a **frozen store**: the store as it stood when the
round began. A **shared round** is a deferred round whose work is divided
among the threads: its first scans are cut into pieces, workers derive
from their pieces and set aside the tuples the store lacks, and the merge,
itself shared among the workers, inserts them exactly as the serial evaluator
would have, in the same order and with the same identifiers. Relations,
answers, diagnostics and every counter but `parallel_rounds` are therefore
the same at every thread count. A round never fails where the serial
evaluator would not: whatever goes wrong on a worker, the round is rerun
serially. The store's read-only promise during a round is carried by the
type system (`lib/frozen.h`) rather than by a list of callers.

Computed values stay worker-local during derivation. The merge gathers
one descriptor per referenced worker-local value in logical first-occurrence
order, then uses the ordinary value resolver to assign identifiers. Private
columns are rewritten directly to those promises before tuple deduplication
and commit. This preserves value identity without another interner and
bounds resolution descriptors by local distinct values rather than column
occurrences.

See [design-parallel.md](design-parallel.md) and
[pending value compaction](design-pending-values.md).

### Library interface

`<relic.h>` is the whole interface, and the tool reaches the engine only
through it. A caller creates a context, loads text or imports tables, adds
facts, prepares queries and runs them into a row callback or collects them
into a result, and reads diagnostics through a callback or the last error
and its class. A collected result owns the bytes of its symbols and
strings, under identifiers no context assigns; a row callback's values are
borrowed for the call. Caller code that runs inside an operation -- a
callback, or the read function of a stream being loaded -- may only read
through the context and interrupt it; any other call on the busy context is
refused, so re-entry cannot corrupt an operation in progress. Entry
points, ownership and callback rules are in relic(3).

See [design-api.md](design-api.md).

### Command line

The tool (`cmd/relic`) is an ordinary library client. An invocation runs in
phases -- command line, loads, imports, check, preparing every query and
export, evaluation, exports, then any interactive session -- each only if
the one before it succeeded, so evaluation starts only once the whole input
has been accepted. Results go to standard output or
the `-o` file; diagnostics and `-e` text go to standard error, so standard
output carries only results in every format. Options are single letters
parsed by `getopt(3)`. Resource limits share one repeatable `-l key=value`
option so that a new limit never consumes a letter, and `-X` selects
internal strategies so that the differential suite drives every strategy
through the ordinary interface rather than a private entry point. The
interactive session (`-i`) uses `libedit` when present, and catches
`SIGINT` only while it answers a line. The options are in relic(1).

See [design-cli.md](design-cli.md).

### Tables

`-I` imports a csv or tsv table as facts of a predicate, with a declared
type per column (from the import or from the predicate's `-T`); nothing is
inferred. Program text cannot name a file, because a program is untrusted
input and must not open a path of its choosing. Each input format is the
exact inverse of the output format of the same name, the first mistake ends
an import, and an import commits through the same all-or-nothing commit a
load uses. Bounded batch decoding and scalar replay share the format's
state machine ([bulk import](design-bulk-load.md#bounded-table-input)).
`-O` exports a relation as exactly the answer to a query with a
distinct variable in every column, in csv, tsv, json or `dl`, the last
loading back as a program with every value's type intact.

See [design-tables.md](design-tables.md).

## Query modes

A query is answered in one of two ways, and both give the same set of rows.

**Query-directed** is the default for a query. The derived relations are
discarded; the predicates the query reads completely are evaluated from the
program's own schedule; the Magic rewrite generates rules specialized to
the query's bindings; those are stratified, planned and evaluated with the
ordinary semi-naive engine; and the query's rule runs last. What the
rewrite derives is only part of the fixed point, so it is not recorded as
the program's fixed point, and the next evaluation starts again from the
facts.

**Whole-program evaluation** computes the complete stratified fixed point
and runs the query over it. It is used:

* when the caller asks for it, with `-a` or
  `relic_materialize()`;
* when the rewrite is turned off, with `-X no-magic` or
  `RELIC_F_NO_MAGIC`;
* when the relations already hold the program's fixed point, so the query
  reads them directly without a rewrite;
* when the rewrite outgrows its limit: it is abandoned with a warning and
  the query answered from the whole program. The rewrite is an
  optimization and must not decide whether a query is answered.

Between the two, a rewrite in which no adornment binds a column generates
nothing: the query runs as written over the program's own relations,
evaluated from the program's schedule for the components it reaches.

The two modes give the same answers because the rewrite computes exactly
the tuples of the fixed point that the query reads. They may differ in
whether they fail: the whole program evaluates more bindings, and may meet
an evaluation error or a limit that the rewrite would not have reached.

In either mode a query changes neither the program nor what a later query
answers, but the values, predicates and counts it interns persist for the
life of the context ([growth for
life](design-values.md#growth-for-the-life-of-the-context)), so memory
headroom and counters may depend on earlier runs.

## Contracts

### Naming

The tool is `relic`, the library `librelic`, the public header `<relic.h>`,
the pkg-config module `librelic`, and the exported prefixes `relic_` and
`RELIC_`. Internal symbols with external linkage also use `relic_` but are
hidden; file-local symbols are `static`. Program files conventionally end
in `.dl`.

An unrelated cryptographic toolkit also publishes a `librelic` and a
`relic.h`. relic installs its header directly as `$(includedir)/relic.h`,
so the collision appears only when both are installed under one prefix;
the remedies are `--program-prefix` and a renamed pkg-config module, and
neither touches the source tree. This is flagged, not designed around.

Names the engine generates -- a query's `$q<serial>`, `path$bf`,
`magic$path$bf`, `walk$path$bf`, `sup$...`, `agg$...`, `p$base` -- contain `$`,
which no program identifier may, so a collision with a user predicate is impossible
by construction rather than avoided by a uniquifying counter, and the names
stay readable in `-e` output.

### Determinism

Answers are sets and therefore unordered, but relic produces deterministic
plans and a deterministic derivation order for identical input:

* value identifiers, within each type, and predicate identifiers are
  assigned in order of first appearance; a load interns the predicates of
  its facts as it reads them;
* the undefined-predicate warning points at the first atom that reads the
  predicate, and such warnings come in predicate-identifier order;
* relations iterate in insertion order: a base store in the order the text
  states its facts, a derived relation beginning with the facts seeded from
  its base store;
* the planner breaks ties by source position, and its estimates are
  integer arithmetic;
* generated predicates are named from their source and adornment, never
  from a counter; a query's answer relation, which is no predicate, is
  `$q<serial>`, numbered in the order queries are prepared;
* no result and no `-e` line depends on a pointer value;
* nothing depends on the thread count (see [Threads](#threads)).

Two runs over the same input therefore produce byte-identical `-e` output
and byte-identical unsorted results. This is a testability property, not a
convenience: the golden test suite and the differential suites depend on
it.

The hash seed is deliberately outside this promise. Every hash table
answers which identifier a key has and is never walked in slot order, so
the seed decides where a key sits and how many probes find it, and nothing
any interface reports: no result, `-e` line, identifier, table capacity or
memory charge. A context therefore draws a 64-bit seed from
`getentropy(2)` when it is created, and creation fails rather than falling
back to a constant when no entropy is available. With a seed fixed at
compile time, a program author could compute every hash and craft input
that drives a relation into one probe cluster, making its load quadratic
inside every limit.

`-s` sorts rows for scripts that must not depend on derivation order.
Because a column may hold more than one type, it uses a total order: by
type rank, integer then symbol then string, and within a type by the
language's [ordering](language.md#values-and-types). This order exists only to
stabilize output and is invisible to the language.

### Errors and exit status

Public functions return `0` or `-1`, or a pointer or `NULL`. Every failure
is explained by an error diagnostic, delivered to the context's callback
(except for a call refused because the context was busy), and
`relic_error()` returns the last error's text. `errno` is never overloaded
for semantic errors and is preserved where a system call failed.

Every error also carries a class, returned by `relic_error_class()`. The
class says what the caller can do about the failure -- fix the input,
raise a limit, check a file, free memory, report a bug -- so a
program acts on the class and the text stays a message for a person;
nothing in the library's contract depends on message wording. The library
reports an error only through functions that take its class, so no error
can be recorded without one. The class and the text share one lifetime,
set before the callback runs. The enumeration is part of the interface: a
value is never renumbered or removed.

The library never writes to standard error on its own. Where a diagnostic
goes is the caller's choice, made by installing a callback; the library
offers `relic_diag_stderr()`, a formatter a caller may install, and the
tool is an ordinary caller that installs it. `err(3)` and its variants
belong to the tool alone.

The tool has three exit statuses: 0 for success, 1 for any failure after
the command line was accepted, and 2 for a usage error; warnings do not
change the status, and it does not follow the error class (see [exit
status](design-cli.md#exit-status)).

See [design-api.md](design-api.md#errors) for the classes.

### Memory and failure

Lifetime, not size, decides how memory is allocated. What is created in
large numbers and dies together -- parsed statements, compiled rules, the
schedule, a query's rule, a rewrite, plans and join state -- comes from an
arena of that lifetime, freed in one operation. What outlives queries,
must shrink, or is counted against a limit item by item -- relation tuples
and indexes above all -- is a growable array over the system allocator.
There is no `malloc()` per syntax node or per tuple.

An arena allocation that fails returns `NULL` and sets a sticky flag on the
arena. Callers check the pointer as usual; the flag lets a deep helper that
has already failed return early without inventing an error path, and lets
each phase confirm at its boundary that nothing failed. Every allocation
failure is reported as an ordinary diagnostic and a failure return. The
library never aborts on allocation failure: a library must not terminate
its host.

`max_memory` is a ceiling on accounted storage and evaluation memory, not on
the process. It charges what grows with the data or with the work: relation and
index storage, the symbol pool, the integer table, the name store, the buffers
evaluation computes strings in, and, each as a whole, the arenas of an
operation's lifetime -- an evaluation's plans and a query's rewrite. The arenas
that hold the program, the schedule and a prepared query are not charged,
because the caller wrote what they hold and a query may outlive its context. A
refusal by the limit is reported through the same function an exhaustion is,
with the limit's message.

A context's tables -- the symbol pool, the value tables, the predicate
table and the name store -- grow for the life of the context, and nothing
un-interns a value or a name; `max_memory` counts that growth and refuses
it at the limit. Why reclamation is not worth its mechanism is in [growth
for the life of the
context](design-values.md#growth-for-the-life-of-the-context).

See [design-memory.md](design-memory.md).

### Limits

Limits protect command-line and embedded users from accidental combinatorial
explosion. The configurable limits are on tuples across all relations, on
memory (see [Memory and failure](#memory-and-failure)), and on the work of one
query's Magic rewrite. They are fields of `relic_config_t`, and the tool's `-l
key=value` keys correspond to them one to one: a limit is defined in one place,
and the tool only parses into it. A zero tuple or memory limit means unlimited.

An exceeded limit is an ordinary error, never an abort. The diagnostic names
the limit, its configured value and the operation that exceeded it -- a
predicate, an aggregate of one, or the query:

```text
relic: error: limit exceeded: tuples=N while deriving p/2
```

Partial results are discarded: a query that exceeds a limit delivers no rows
and fails, because a truncated answer set would be indistinguishable from a
complete one. A load, an import or an added fact that does not fit is
refused with nothing added. No strategy switch changes an answer, but two
strategies may hold different amounts while computing one, so one may meet
a limit the other does not. The symbol pool is charged in full, literals
and computed strings alike, because a rule such as
`s(Y) :- s(X), Y = cat(X, "a")` interns a string per derivation.

The Magic limit is the exception. Left unset it is not unlimited but in
proportion to the program and the query, and no value removes it. A rewrite
that outgrows it, set or not, is abandoned and the query answered from the
whole program, which answers the same; a caller who wants no rewrite has
`-X no-magic`. One count, in literals, meters the rewrite's search and its
generation alike, so every predicate and rule it generates is bounded through
the literals it took, and no configuration leaves a stage unmetered.

Structural limits are fixed (see [Limits](language.md#limits)). Two
shape the engine: a predicate has at most 32 columns, so an index key is
one 32-bit column mask; and a rule has at most 64 distinct variables, so a
binding frame is a plain array with a 64-bit bound mask that the join loop
never allocates. The variable limit is hard rather than a fallback to a
dynamic bitmap: it is generous for Datalog, and a fallback would be a
second code path on the hottest loop. Exceeding either is a static error
naming the rule.

### Threads

A `relic_t` is not thread-safe: it is used by one caller thread at a
time, and caller code running inside one of its operations may only read
from it or interrupt it. Distinct contexts share nothing: the library has
no mutable global state, which `tests/globals.sh` enforces by failing when
the built library defines a symbol in a writable section.

A context may own worker threads, sized by `threads` in its configuration
and by the tool's `-j`. They start the first time an evaluation shares a
round, run only inside an operation the caller started, call none of the
caller's callbacks, block every signal but those a fault in them raises,
and are joined when the context is destroyed. To its caller the context
is used by one thread at a time. A thread that cannot be started is
not an error; a child forked between operations may go on using the
context on the calling thread alone. A build without POSIX threads or C11
atomics, or configured `--disable-threads`, runs every round on the
calling thread.

`relic_interrupt()` may be called from a signal handler, another thread or
a callback: it raises a flag and can neither block nor fail.

### Portability

Primary platforms are FreeBSD, OpenBSD, NetBSD and macOS; Linux with glibc
and with musl is supported as well.

The code is ISO C11, compiled as `-std=gnu11` where available, since a
strict-conformance switch hides BSD interfaces such as `strlcpy(3)` behind
feature-test macros on several platforms, falling back to `-std=c11` and
then the compiler's default. Exactly one dialect switch reaches the
compiler, so every platform sees the same language. No variable-length
arrays, `alloca`, nested functions or statement expressions.

Availability is decided by `configure`, never by a platform test in the
engine. A missing function is replaced with `AC_REPLACE_FUNCS` from
`compat/`, compiled only where it is needed, so engine sources call
`reallocarray()`, `strtonum()`, `strlcpy()`, `memmem()` or `getentropy()`
unconditionally. The rules:

* every `.c` file includes `config.h` first;
* `include/relic.h` never includes or depends on `config.h`;
* no source file outside `compat/` contains a platform conditional;
* no broad portability framework is introduced.

`queue.h` is bundled rather than probed. It is small and header-only, and
the macro set of the system copy differs between glibc, musl, macOS and
the BSDs in ways that are tedious to probe correctly and easy to probe
incompletely; one known copy everywhere removes that class of defect and
makes list behaviour identical on every platform. It keeps its upstream
3-clause BSD notice, and `compat/README.md` records each copy's origin,
revision, checksum and patches.

The implementation prefers BSD facilities where they simplify: `getopt(3)`,
`err(3)` and its variants in the tool, `reallocarray(3)`, `strtonum(3)`,
`getentropy(2)`, `bzero`, `bcopy` and `bcmp`, and the `queue(3)` macros
for worklists. Hot lookups -- symbols, values, predicates, relations -- use
purpose-built open-addressing tables storing 32-bit identifiers inline,
not trees.

### Dependency policy

relic has no third-party runtime dependencies. The engine and the tool use
the C11 standard library and BSD interfaces, and nothing else. This is a
product requirement, not an aesthetic preference: the tool must be as easy
to install and audit as `awk` or `grep`.

In order of preference:

1. **Use the system interface.** If libc or a BSD interface does it, call
   it.
2. **Compile a compatibility object.** If a platform lacks that interface,
   `configure` selects a replacement from `compat/` for that platform only.
3. **Vendor a pinned upstream release.** If a component genuinely needs
   external code, vendor a specific released version under `vendor/`,
   keeping its license, provenance and checksum, keeping the integration
   layer separate from upstream sources, documenting every patch, and
   building it under the same warnings and sanitizers as the rest of the
   tree.
4. **Write it.** A purpose-built open-addressing hash table storing 32-bit
   identifiers inline is a few hundred auditable lines and faster than a
   generic one. Reimplementing a general-purpose library would not be.

The string function `index` is an instance of the second rule: it is
built on `memmem(3)`, with a replacement in `compat/` where it is missing.
`match` is an instance of the fourth: `lib/regex.c` is relic's own matcher,
because `<regex.h>` would make a pattern's meaning depend on the platform's
engine and locale and its running time on that engine's backtracking, and a
vendored engine would cost more to audit than the matcher costs to write.

The one optional dependency is `libedit`, used only for line editing and
history in the interactive session. `configure` detects it; it is never
required, and its absence changes only how the session reads input.

Build and maintainer tools are a separate question from runtime
dependencies. No parser generator is used at any time, so no generated
source is committed and the grammar lives where its diagnostics do.

### C library and ABI

Every major object -- the context, a query, a collected result -- is an
incomplete struct with a matching `_t` typedef, so callers cannot depend on
its layout. Five types are complete because a caller constructs or reads
each directly and an accessor per field would carry nothing the struct does
not: a value, a source location, a configuration, an import description,
and the statistics counters. The counters are a fixed, small set read at
one moment, which is why they are a struct; rows and diagnostics are
unbounded and arrive while work is in progress, which is why they are
callbacks. What the counters count depends on the evaluation strategy and
is not a stable interface.

The header is strict C11, includes nothing internal and not `config.h`, and
is guarded for inclusion from C++. Only symbols matching `^relic_` are
exported, the library is compiled with hidden visibility where the compiler
supports it, and it is linked with `-no-undefined`, so a shared build fails
loudly on a platform that requires a library to name every library it
needs.

The shared library carries a libtool `-version-info`, maintained in
`configure.ac` apart from the release version by libtool's rules (see
[versioning](design-api.md#versioning)).

See [design-api.md](design-api.md) and relic(3).

### Build

A release tarball builds with a C11 compiler, `make` and a POSIX shell:
no autotools, Python, perl, CMake, Meson or network access. The tarball
from `make dist` carries the generated `configure` and `Makefile.in`.
Regenerating them from a checkout needs autoconf 2.69, automake 1.13 and
libtool 2.4.2 or newer, through `./autogen.sh`; the macros `configure`
needs beyond autoconf's own live in `m4/` rather than coming from
autoconf-archive, so no fourth package is needed.

```sh
./configure && make && make check && make install
```

Out-of-tree builds are supported and preferred, and `make distcheck` must
pass. The build is one non-recursive `Makefile.am`: the tree is small, a
non-recursive build is faster, and one Makefile makes the dependencies
between library, tool and tests correct by construction rather than by
convention. A hand-written BSD `Makefile` is not maintained beside it: the
autotools build is the single source of truth, so every ports tree and
package system consumes one tarball with one configure interface, and
packaging skeletons live in `contrib/` rather than forming a second build.
The tool lives in `cmd/relic/` rather than `usr.bin/relic/` for the same
reason.

`./configure --help` lists the options: debug builds, `-Werror`, threads,
each sanitizer, coverage, fuzzers and `libedit`. Every installed path
derives from an autoconf directory variable, so `--prefix`, `--libdir`,
`--mandir` and `DESTDIR` relocate the whole install. `make check-asan`,
`check-ubsan`, `check-lsan`, `check-msan` and `check-tsan` run the suite
under each sanitizer, `check-random` runs the randomized differential
suite over many seeds, and `coverage` writes a coverage report; a target
unsupported on the platform says so and does nothing (see
[design-testing.md](design-testing.md)).

Continuous integration builds the tarball with no autotools installed,
because that is the path users take and the one that breaks silently.
Every job passes on the commit a release is made from, and a job that
cannot run what it is for fails rather than skips: `RELIC_STRICT_CHECK`
turns every skipped test into a failure, except those a job names in
`RELIC_ALLOWED_SKIPS`. The matrix and the release
procedure are in [design-release.md](design-release.md).

### Performance targets

relic should comfortably handle 100,000 to 1,000,000 facts, hundreds of
predicates and hundreds of rules, and recursive graph workloads, without
pathological overhead. For selective graph queries, the Magic rewrite
should avoid materializing a global transitive closure. No benchmark
number is a correctness requirement, but the architecture must make
performance measurable and improvable: `-X stats` and `relic_stats()`
report the work an evaluation did, and the benchmark harness compares
relic with other Datalog and tabled-Prolog engines on answers as well as
time (see [design-bench.md](design-bench.md)).

## Source layout

```text
configure.ac, Makefile.am   the build: one autoconf input, one
                            non-recursive automake input
autogen.sh                  maintainer bootstrap
librelic.pc.in              pkg-config template
m4/                         local autoconf macros
include/relic.h             the public header, the only one installed
lib/                        librelic: front end, check, analysis,
                            planner, executor, evaluator, rewrite, store
cmd/relic/                  the command-line tool
compat/                     replacements configure selects, bundled
                            queue.h
man/                        relic(1) and relic(3)
examples/                   small librelic consumers
tests/                      unit, language, equivalence, random and fuzz
                            suites and the scripts make check runs
bench/                      benchmark workloads and timing harness
contrib/                    packaging skeletons
specs/                      design documents (see README.md)
docs/                       category documents of TODO.md
.github/workflows/          continuous integration
README.md, EXAMPLES.md      user documentation
NEWS, TODO.md               release notes; open work
LICENSE                     the 2-clause BSD license
```
