# Language and tooling gaps

What other Datalog systems offer that relic does not, sorted into what is
worth adding and what is worth measuring first. What relic leaves out on
purpose is in architecture section 3.
The engines compared are the ones a relic user might otherwise reach for:
Souffle, LogicBlox's LogiQL, Nemo, clingo, CodeQL, DDlog and the tabling
Prologs. Each item names the engines that have the feature, what a relic
program does without it, and the reason for the verdict. Verdicts are
provisional until a design exists.

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
