# Performance plan

[Medium, components: `lib/exec.c`, `lib/round.c`, `lib/task.c`,
`lib/merge.c`, `lib/relation.c`, `lib/import.c`, `bench/compare/`]

Make complete commands faster and parallel working memory more predictable,
while keeping one understandable execution and ownership model. The
reference is the simplified engine, with the default one-thread path given
the same weight as useful parallel execution.

The [scan-only comparison](../bench/compare/recorded/scan-only.md),
[rough rival refresh](../bench/compare/recorded/m3-max/rough-rivals/README.md),
[completed investigations](../bench/compare/recorded/focused-investigations.md)
and [complexity audit](../bench/compare/recorded/complexity/README.md) supply
evidence. Use the
[measurement protocol](todo-performance-measurements.md) for the work below;
[parallel issues](todo-parallel.md) describe additional symptoms, not a
separate implementation queue.

## Characterize the reference

Start from the scan-only engine: partition a leading scan and run other
plans whole. Its bounded recursive selector, pending-value compaction,
narrow query factoring, transferable load payloads and ordinary fold/EMIT
ownership form the reference, not separate implementation stages.

Use the scan-only candidate identified in that report as the initial
control, preserving its frozen source, builds, fixtures and answers.
Reuse its full normal/sanitizer checks and one/sixteen-worker facts
import/join and cyclic/chain comparisons.
The corresponding p620 comparison remains open until that host is usable;
CPU starvation is not a reason to repeat the completed Mac cohort.

Use the existing benchmark families to map the remaining costs: ingestion,
bound and unbound graph queries, program analyses, and small commands.
The rough rival comparison locates the largest remaining complete-command
gaps in grouping/join throughput, full closure and parallel memory; its
two-run observations do not select an implementation or locate a live owner.
Reuse valid evidence and profile only where the simplified engine makes
the old attribution uncertain. Rank work within each family; an aggregate
score must not hide a default-path regression or a large memory cost.

Exit with a ranked cost map and a concrete target, control and rejection
criterion for the next change. Keep the initial baseline as the control
for the final composition comparison. Close retained correctness gaps only
where existing evidence is insufficient; removed machinery creates no
validation work.

## Use existing mechanisms first

Make these two decisions independently, in separate changes:

* **Derived-relation tags.** Validate the existing identifier table's tag
  choice on the simplified baseline, including a same-generation control,
  a wider tuple and a large-storage case. Its measured closure gains make
  it the strongest small candidate, but its persistent storage cost must be
  explicit. Select it only if the additional memory and complete-command
  results are acceptable; do not build another container to rescue it.
* **Expired worker screens.** Release screens at the existing gather
  barrier if the lower memory-limit requirement and simpler lifetime earn
  the change. Retain the failing-before/passing-after admission regression
  and check throughput controls. Its benefit is reduced charged overlap;
  process RSS and speed are separate observations.

After either change is accepted, it becomes part of the working reference
for the next local comparison. After both decisions, measure their actual
combined result against the initial baseline. Do not multiply
isolated improvements or carry unused strategy switches into production.

## Reduce parallel working memory

The main unresolved memory problem is the accumulation of copies of the
same candidate across workers. Freeing screens does not remove that bound.
The objective is to reduce peak growth as worker count rises while keeping
useful parallel speed, deterministic identifiers and ordinary serial retry.

First measure candidate rows, copied value bytes, screening and merge
scratch at their actual lifetime boundaries on the new reference. Use one
duplicate-heavy target and one low-duplicate/computed-value control. Compare
one, four and sixteen workers before increasing dataset size.
Distinguish unused reservation and overlapping lifetimes from rows that
must remain live: each calls for a different intervention.

Try the smallest change that removes demonstrated duplication or overlap.
If that cannot address the dominant term, write one focused design with an
explicit byte bound, a single ownership boundary and a publication/failure
proof before implementing one prototype. Compare its complete-command cost
and peak on both controls. A protocol that adds new context-wide states,
interner ownership or a second execution model needs benefits that justify
that scope. Reject or narrow it when they do not. A general routing/wave
framework is not the starting design.

## Decide the parallel import tradeoff

The isolated comparison establishes a real import cost from removing the
symbol package. Decide whether that throughput is worth its maintenance
cost on the simplified reference, separately from aggregation.

Use the existing evidence to choose between retaining the single-index
tradeoff and testing restoration of the package alone. Any restoration
must preserve default serial performance, deterministic value numbering,
limits and refusal behavior. Measure files and facts imports and complete
commands, with an integer-only import control. Report RSS and charged
storage separately. A smaller publication alternative is worth prototyping
only when a profile identifies the work it would remove.

The scalar reservation shortcut has inconclusive timing evidence and adds
no task. Import ownership attribution is complete: the former 120 MB target
is not a cleanup-sized change with the measured live representation. There
is no allocator workaround or additional delay experiment to implement.

## Consolidate and stop

After the selected changes, run the representative complete-command cohort
against the initial frozen baseline and relevant competing engines.
Record default latency, parallel speed, peak/accounted memory, answers and
the permanent code/ownership cost together. Finish the correctness gaps
relevant to the retained paths, including plan/index refusal, rewrite limits
and reuse, import fallback and input limits.

Publish one result and update the reference. Choose another optimization
only if that result identifies a material remaining bottleneck and a
bounded intervention. An inconclusive experiment ends with its evidence;
it does not automatically produce another heuristic or architecture.

General grouped reduction, new grouped/cached-hash membership containers
and a replacement routing framework have no implementation commitment.
ThinLTO/build-policy validation remains a separate measured choice, with
compiler and alignment policy held fixed during the engine comparisons.
