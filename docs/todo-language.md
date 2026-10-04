# Language and tooling gaps

What other Datalog systems offer that relic does not and is worth adding.
What relic leaves out on purpose is in architecture section 3.
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
rules: a rule file names other rule files, not data. It is spelled
`#include`, which D-30 reserves. The safe form resolves a name relative
to the including file, refuses `..` and absolute paths, and bounds the
depth.

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

**Floating point.** [Low, component: language, `lib/value.c`] Numbers with
a fraction: a ratio, a weight, a measurement imported from a table.
Souffle, LogiQL, Nemo, CodeQL, DDlog and the Prologs have a float type;
clingo, like relic, has integers only. A relic program scales to an
integer, cents or parts per thousand, and divides with truncation. The
design is `specs/design-floats.md` and the decision D-33: a fourth type
equal only to itself, ordered exactly against the integers, an exact
`total` aggregate, and what keeps every platform's bytes the same. It
stays out of version 1, architecture section 3, and comes last in
section 69 because no workload measured here asks for it; the design is
recorded so that a later version, or a language built as a superset of
relic's, adds floats in one way.
