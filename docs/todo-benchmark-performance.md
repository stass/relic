# Performance work

[Medium, components: `lib/eval.c`, `lib/import.c`, `lib/facts.c`,
`lib/relation.c`, `bench/compare/`]

Use the accepted dense-collection engine as the native control. Its
[recorded comparison](../bench/compare/recorded/dense-rounds/README.md) includes
the missed 90% RSS aim and the remaining roughly twofold memory gap on the fresh
FreeBSD rival comparison. The
[earlier performance pass](../bench/compare/recorded/performance-pass.md)
retains the other rival gaps and rejected approaches.

## Choose the next gap

Files/facts throughput and serial closure time remain material classes.
Choose one representative cell by absolute excess time and bytes, using the
existing rough comparison. Attribute its remaining cost to parsing, value
assignment, relation/index work or executor work before proposing code.
Further dense-memory work needs attribution of the common append and storage,
which outlive the collector arrays.

Define one bounded change around the measured owner or operation. State what
storage, traversal or mechanism it removes, its source budget, its controls
and its rejection criterion before implementation. Preserve complete answers,
order, assigned identifiers, counters, final accounted storage and refusal
behavior. Require actual successful execution of the changed path.

## Decide and validate

Start with a small fixed screen, then use the
[measurement protocol](todo-performance-measurements.md) for a decisive
comparison only if the screen shows a useful gain. Retain every observation
and failed attempt. Measure the accepted changes together, compare rivals
on the same host and output policy, and run the required correctness and
sanitizer checks. Close an unproductive candidate; keep unresolved toolchain
validation work in the testing category of `TODO.md`.
