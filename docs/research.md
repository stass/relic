# Research and methods used in relic

This index connects papers, reference implementations and local methods to
the parts of relic they explain. It is a starting inventory, not a claim of
complete historical attribution. A method reference explains an algorithm;
it does not by itself establish that relic copied that source or implements
every variant in the paper.

**Used** means the method is present in runtime code. **Proposed** means it
has a design but is not implemented. **Related** means it informs comparison
or design review without being relic's implementation. Local adaptations and
their proof obligations are identified separately from the external method.

## Evaluation and query rewriting

### Semi-naive fixed points — Used; method reference

Serge Abiteboul, Richard Hull and Victor Vianu, *Foundations of Databases*
(1995), [chapter 13, section 13.1](https://webdam.di.ens.fr/Alice/pdfs/Chapter-13.pdf).
Delta relations avoid repeating derivations using only old tuples.

In relic: [evaluation design](../specs/design-eval.md#the-semi-naive-fixed-point),
[lib/eval.c](../lib/eval.c). The implementation partitions work through delta,
old and current views and retains a naive evaluator as an independent
reference. Those exact view and ordering contracts belong to relic's design.

### Magic Sets and sideways information passing — Used; method reference

Catriel Beeri and Raghu Ramakrishnan, *On the Power of Magic* (PODS 1987),
[publication](https://doi.org/10.1145/28659.28689),
[author-institution abstract](https://cris.huji.ac.il/en/publications/on-the-power-of-magic-2/).
Bindings are propagated by rewriting demand into rules evaluated bottom-up.

In relic: [Magic design](../specs/design-magic.md),
[lib/magic.c](../lib/magic.c), [lib/plan.c](../lib/plan.c). Adornments and
supplementary predicates share the planner's binding order. Relic's treatment
of complete inputs, stratification, limits and abandonment is a local
contract, not a claim that the cited paper proves every supported extension.

### Factoring bound recursive queries — Used; explicit design source

Jeffrey F. Naughton, Raghu Ramakrishnan, Yehoshua Sagiv and Jeffrey D. Ullman,
*Efficient Evaluation of Right-, Left-, and Multi-Linear Rules* (SIGMOD 1989),
[paper, section 2](https://sigmodrecord.org/publications/sigmodRecord/8906/pdfs/66926.66948.pdf).
The relevant construction reduces the recursive state carried for a bound
query.

In relic: [factoring design](../specs/design-query-factoring.md),
[lib/magic.c](../lib/magic.c). The design explicitly extends the single-binding
construction with origin columns for correlated requests. Its certificate
and local proof determine eligibility; the paper's broader rule classes do
not automatically qualify.

### Strongly connected components — Used; method reference

Robert E. Tarjan, *Depth-First Search and Linear Graph Algorithms*
(SIAM Journal on Computing, 1972), [publication](https://doi.org/10.1137/0201010).

In relic: [stratification design](../specs/design-stratify.md),
[lib/depgraph.c](../lib/depgraph.c), [lib/stratify.c](../lib/stratify.c).
An iterative Tarjan traversal uses an explicit stack. Component completion
order feeds scheduling; strict dependency edges inside a component identify
forbidden recursive negation or aggregation. Diagnostic selection is local.

## Expressions and hashing

### Thompson regular-expression simulation — Used; method reference

Ken Thompson, *Regular Expression Search Algorithm* (CACM, 1968),
[paper](https://www.jmeiners.com/efficient-programming-with-components/papers/regular-expressions.pdf),
[DOI](https://doi.org/10.1145/363347.363387).

In relic: [expression design](../specs/design-expressions.md),
[lib/regex.c](../lib/regex.c). The matcher simulates a Thompson instruction
program with explicit thread lists. Relic defines its own dialect, bounded
compilation, leftmost-start result and interruption rules; it does not use
the paper's target-machine code generator.

### Integer mixing components — Used; implementation references

Austin Appleby's [MurmurHash3 reference source](https://github.com/aappleby/smhasher/blob/master/src/MurmurHash3.cpp)
defines the 64-bit `fmix64` avalanche operation. Sebastiano Vigna's
[SplitMix64 reference](https://prng.di.unimi.it/splitmix64.c) contains the
`0xbf58476d1ce4e5b9` multiplier used in relic's fold.

In relic: [value hashing](../specs/design-values.md),
[lib/util.c](../lib/util.c). Relic combines a keyed multiplicative fold with
the MurmurHash3 finalizer; its wide hash has a separate unkeyed stripe fold.
It implements neither the complete MurmurHash3 algorithm nor a SplitMix
random generator. These references do not establish cryptographic security
of the composition; its threat model and rationale are stated locally.

## Parallel ownership and storage

### Frozen reads and ordered publication — Used; local method

[Parallel design](../specs/design-parallel.md),
[pending-value design](../specs/design-pending-values.md),
[lib/round.c](../lib/round.c), [lib/task.c](../lib/task.c),
[lib/merge.c](../lib/merge.c).

Tasks read fixed views while private candidates wait for ordered publication.
The frozen header boundary prevents workers from mutating the store. This
preserves serial identifiers, diagnostics, counters and refusal behavior.
There is no claimed external paper for the complete combination. The
[measured memory costs](../bench/compare/recorded/m3-max/merge-screen-reuse/README.md)
are implementation evidence, not an inherent lower bound for determinism.

### Souffle's concurrent B-tree — Related; external implementation

Herbert Jordan, Pavle Subotić, David Zhao and Bernhard Scholz,
*A Specialized B-tree for Concurrent Datalog Evaluation* (PPoPP 2019),
[paper](https://souffle-lang.github.io/pdf/ppopp19.pdf).
Optimistic node locking and traversal hints support shared set insertion.

Relevant to [candidate ownership](../specs/design-dense-rounds.md#alternatives-not-taken).
Relic does not implement this tree. Its failed mutex collector does not
invalidate this synchronization design. The paper does not provide relic's
serial occurrence order or establish the result of a relic prototype.

### Brie and dense membership — Related; external implementation

Herbert Jordan, Pavle Subotić, David Zhao and Bernhard Scholz,
*Brie: A Specialized Trie for Concurrent Datalog* (PMAM 2019),
[paper](https://souffle-lang.github.io/pdf/pmam19.pdf).
Shared prefixes and membership bits compress dense relations; insertion uses
atomic updates, with lock-free claims conditional on the atomic support.

Relevant to the same [dense collection design](../specs/design-dense-rounds.md).
Relic does not implement Brie. Souffle documents it as an optional
[relation representation](https://souffle-lang.github.io/relations); citing it
does not establish that a particular retained benchmark used it.

### Earliest-occurrence ranks — Proposed; local adaptation

[Dense round design](../specs/design-dense-rounds.md),
[abstract model](../bench/compare/recorded/dense-round-design/README.md),
[implementation work](todo-parallel.md).

A fixed cell per possible pair stores an atomic minimum occurrence rank;
sorting surviving cells reconstructs serial insertion order. The local proof
uses monotone task assignment and a bounded worker sequence. The shared-set
papers motivate the ownership direction but do not prove this algorithm.
There is no runtime implementation or measured benefit claimed here.

## Maintaining this index

For each new paper-backed method, record the full reference and a primary
link, the status, the exact idea used, the owning source/design, and material
differences or assumptions. Keep performance claims with their experiment
links and conditions. Mark related reading as related, and local methods as
local; do not invent provenance to fill an entry. Update the entry when its
implementation or applicability changes. Git retains the historical trail.
