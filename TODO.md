# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Implement relic 1.0.** [Critical] Fifteen stages S0 through S14, specified in
`relic-plan.md`. Current position: nothing started, S0 next.

## Documentation debt

**Design documents owed.** [High] Each stage owes one before its code:
`specs/design-memory.md` (S1), `design-parser.md` (S2), `design-program.md`
(S3), `design-relations.md` (S4), `design-plan.md` and `design-eval.md` (S5),
`design-stratify.md` (S6), `design-index.md` and `design-planner.md` (S9),
`design-magic.md` (S10). Each is referenced from `specs/architecture.md` when
written.

**`specs/style.md` readability references.** [Low] The readability standard
lists qualities to aim for but names no relic functions yet, because none
exist. Populate the reference table once S5 lands, choosing functions that
actually read well rather than whichever happen to be there.

## Engine

**Semi-naive duplicate elimination.** [Low, component: `lib/plan.c`] The delta
variant scheme of D-10 derives some tuples more than once per iteration for
rules with three or more recursive body literals. The refinement that uses the
old relation for earlier literals and the new one for later literals removes
that, at the cost of being non-obvious on first reading. Measure before
implementing; rules that would benefit are rare.

**Append-only invariant versus incremental maintenance.** [Medium, component:
`lib/relation.c`] D-10 represents a delta as a range of tuple identifiers,
which requires that nothing compacts, deletes or renumbers a relation during a
fixed point. Any future incremental insertion or DRed-style deletion support
must confront this first. The invariant is asserted in `lib/relation.c`.

**Size budget.** [Medium] `relic-plan.md` totals about 7.5 KLOC against the 4
to 7 KLOC of `specs/architecture.md` section 2.2, which treats size as a design
constraint. Track actual lines per stage against the budget; the levers are
listed under risk R7.

## Packaging

**`librelic` name collision.** [Low] An unrelated cryptographic toolkit
publishes `librelic` and a `relic.h`. Only matters if both install under one
prefix. Remedies are `--program-prefix` and a renamed pkg-config module.
Revisit if a distribution reports it.

## Post-1.0

Candidate features, in the priority order of `specs/architecture.md` section
69. None may change the language semantics of v1.

* CSV and TSV fact import
* arithmetic expressions
* incremental insertion maintenance
* DRed-style deletion maintenance
* aggregates
* parallel SCC evaluation
* persistent compiled rule plans
* relation export
* better cardinality estimation
* further Magic Sets and negation optimization
