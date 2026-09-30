# Language and tooling gaps

What other Datalog systems offer that relic does not, sorted into what is
worth adding, what is worth measuring first, and what stays out on purpose.
The engines compared are the ones a relic user might otherwise reach for:
Souffle, LogicBlox's LogiQL, Nemo, clingo, CodeQL, DDlog and the tabling
Prologs. Each item names the engines that have the feature, what a relic
program does without it, and the reason for the verdict. Verdicts are
provisional until a design exists; a "declined" item can be reopened by a
workload that needs it.

The rule the verdicts follow is architecture section 73: relic is one
relational engine with no hidden second one, and its simplicity is the
feature. A feature is worth adding when it changes which programs can be
written and fits the engine as one more operation or one more rewrite; it is
declined when it would make relic a database or a Prolog.

## Worth adding

**Provenance.** [Medium, component: `lib/eval.c`, `cmd/relic/`] Why was this
tuple derived. Souffle's `-t explain`, Nemo's tracing and LogiQL's
provenance answer it; relic's `-e` shows plans, not derivations. For a tool
aimed at dependency and policy graphs, "why is this task blocked" is the
question users ask most. The cheap form is a derivation tree for one tuple,
found after the fact by re-running the rules that could derive it against
the fixed point, which needs no bookkeeping during evaluation and no change
to the store; the expensive form records a justification per tuple and is
also the first step toward deletion maintenance.

**Disjunction in a body.** [Low, component: `lib/parse.c`, `lib/rule.c`]
`a(X); b(X)` as sugar for two rules. Souffle, clingo, CodeQL and the Prologs
have it. Expands at compile time into one rule per combination, so nothing
below the parser changes; the only decisions are the operator, `;` as in
Prolog and Souffle, and the wording of `-e`, which shows the expanded rules.
A body with several disjunctions expands to their product, which the
literal budget already bounds.

**Including another file.** [Low, component: `lib/parse.c`, `cmd/relic/`]
Souffle's `#include`, clingo's `#include`, CodeQL's `import`. Today files
combine only on the command line, so a shared library of rules cannot be
named from a program. The import design keeps data file names out of program
text for reproducibility and safety, and the same argument applies less to
rules: a rule file names other rule files, not data. The safe form resolves
a name relative to the including file, refuses `..` and absolute paths, and
bounds the depth.

**Constraints.** [Low, component: language] A rule with no head,
`:- task(T), not owner(T, _).`, meaning "this must not hold". LogiQL's
`->` constraints and clingo's integrity constraints; CodeQL users write
them as queries. relic can express one as a query that must be empty but
cannot say so in the program. The natural form is a headless rule the check
evaluates like a query and reports as an error with the witness tuple, which
is a small design on top of the query object.

**Relation export.** [Low, component: `cmd/relic/`] Writing a whole
relation, not a query's answer, to a file in one of the output formats.
Souffle's `.output`, and the counterpart of `-I`. Listed in architecture
section 69; the formats exist, so it is a flag and a loop.

**Per-rule profiling.** [Low, component: `lib/eval.c`, `cmd/relic/`]
Souffle's `-p` reports time and tuples per rule; clingo and the Prologs
have statistics per predicate. relic's `-X stats` counts the run as a
whole. The counters exist per plan run already, so a per-rule report is
mostly output, and it is what a user reads before rewriting a slow rule.

## Worth measuring before deciding

**Equivalence relations.** [Low, component: `lib/relation.c`] Souffle's
`eqrel` stores a relation declared symmetric, reflexive and transitive as a
union-find structure, so an alias analysis that would hold a quadratic
closure holds a linear one. Valuable for exactly one shape of program and a
second kind of relation everywhere else in the engine. Measure `cspa`'s
memory against Souffle's with `eqrel` before deciding.

**Incremental insertion.** [Medium, component: `lib/eval.c`] Adding a fact
after materialization discards everything derived. DDlog and LogiQL
maintain the fixed point across changes; Datomic is a database. The cheap
half, a new base fact as a delta of its predicate that the semi-naive
machinery already understands, is item four of architecture section 69 and
is sound for a program without negation; with negation a new fact can
retract a derived one, which is the expensive half. Measure how often a
session adds facts between queries before building either.

## Declined, with the reason

**Type declarations.** Souffle, LogiQL, Nemo and CodeQL declare relations
and column types; relic infers relations from use and leaves columns
untyped, per architecture section 10.1. Declarations would turn an
undefined-predicate warning into an error and a mixed-type comparison into a
compile-time complaint, at the cost of a declaration per relation in every
program, which is most of the text of a small Datalog program. A program
that wants the check can get most of it from `-vv`.

**Floats and unsigned integers.** Souffle and LogiQL have both. Floating
point breaks the determinism of D-4 across platforms and the one
identifier per value of D-3; unsigned integers are a second numeric type
with its own overflow rules for a use no workload has shown.

**Records, algebraic data types and lists.** Souffle records and ADTs,
Prolog terms, clingo function terms. Architecture section 3 rules out
compound terms; they are the line between Datalog and Prolog, and the tuple
store is built on four-byte columns.

**Subsumption.** Souffle's `<=` rules delete a tuple another dominates, for
lattice-style analyses. It conflicts with the append-only store that
semi-naive deltas and the frozen round rest on, `specs/design-relations.md`
section 4.

**Unstratified negation, well-founded semantics, answer sets.** Non-goals
of architecture section 3. clingo exists for the last.

**Persistence and transactions.** Datomic and LogiQL store data; relic
recomputes from files each run, like `awk`. Non-goals.

**Data file names in program text.** Souffle's `.input` and `.output`
directives. Declined in `specs/design-import.md` section 3.3 so that a
program cannot read a file its runner did not name; the README should say
so where a Souffle user will look for it.

**Functions with side effects and foreign calls.** Souffle functors and
clingo's `@` functions. Non-goals; a function with an effect is not a
relation.
