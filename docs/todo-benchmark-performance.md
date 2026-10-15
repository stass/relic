# Performance plan

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`, `lib/exec.c`, `lib/import.c`, `bench/compare/`]

Reduce the remaining cost of useful commands while keeping one understandable
execution and ownership model. Work follows the gaps below, in priority order.
A candidate belongs to a gap; it does not create another implementation track.

## Priorities from the comparisons

The [rough rival comparison](../bench/compare/recorded/m3-max/rough-rivals/README.md)
identifies the large differences. Its two fixed-order runs rank questions;
they do not establish release performance or explain allocation ownership.
Times include loading, evaluation and output. Memory figures are process
peak RSS, in decimal units.

| Priority | Remaining gap | Evidence and reason for the order |
| --- | --- | --- |
| 1 | Parallel working memory | At sixteen threads, cyclic closure peaks at 8.47 GB against Souffle's 0.42 GB and is slower. Acyclic closure and Linux CSPA are faster but use 8.5 and 4.2 times the memory. Capacity and scaling remain problems even when elapsed time wins. |
| 2 | Full-closure evaluation time | Default-thread cyclic/acyclic closure takes 37.0/9.92 s against 23.4/6.76 s: about 13.6/3.2 s excess at scale 4. This is a material latency cost on the default path, with an existing small membership candidate. |
| 3 | Files and facts throughput | At scale 8, grouped files is 2.9/5.3 times slower than DuckDB at one/sixteen threads; facts join is 1.9/2.6 times slower. The larger ratios warrant work, but the measured excess is 0.31-0.85 s and spans several phases. |

Eligible bound-recursion losses have reversed. Keep those gains as regression
controls. Near ties and the roughly 2 ms serial chain-left gap do not justify
new execution machinery. These priorities apply to the measured workloads;
the older dense Andersen/nonlinear cases still need a bounded comparison
before assigning them another optimization track.

## Reference and decision gates

The reference is the scan-only engine measured in the
[trim comparison](../bench/compare/recorded/scan-only.md) and rival refresh.
Keep its frozen source, builds, fixtures and answers as the initial control.
Accepted changes form a working reference for the next isolated comparison;
compare the final combination against the initial control too.

Use the [measurement protocol](todo-performance-measurements.md): reuse the
[completed investigations](../bench/compare/recorded/focused-investigations.md),
resolve the specific missing attribution, test one candidate, then keep or
reject it. Record the expected removed work/bytes and the rejection condition
before implementation. A stronger architecture requires stronger evidence.
M3 work can proceed while p620 is unusable; retain the outstanding p620 trim
comparison and validate selected scheduling/allocation changes there when
conditions permit. A busy-host result remains exploratory.

## Reduce parallel working memory

Start with `tc-cyclic` scale 4 at one, four and sixteen workers, plus one
low-duplicate case that computes values. Reuse the existing ownership
reporter to identify the actual peak's candidate records/columns, screens,
value payloads, find scratch, growth overlap and permanent store. Record
occupied bytes versus capacity, worker copies versus unique new tuples,
charged storage and process RSS separately. The
[scale-2 attribution](../bench/compare/recorded/m3-max/retained-attribution/README.md)
finds about fourteen worker copies per unique tuple in the peak round;
confirm its relevance to the current larger case rather than assigning the
whole 8.47 GB peak to that cause. The [parallel questions](todo-parallel.md)
identify the reservation and scheduling distinctions worth checking.

Then remove the dominant avoidable storage with the smallest ownership change.
If local reservation/lifetime changes cannot address it, write one focused
design for a bound on additional working storage before building one prototype.
Preserve ordered publication, deterministic identifiers and ordinary serial
retry. Show that the bound holds through growth and failure; moving the peak
into another phase or avoiding sharing through fallback does not solve it.

Earlier screen release is a separate small admission improvement within this
work: its scale-2 experiment lowers the charged peak about 13%, while RSS
falls only about 1%. Retain the frozen-budget successful-merge regression
if adopting it. That result cannot close the candidate-replication problem.

Exit with a measured improvement in peak and worker-count scaling, including
useful parallel speed on the target and control, or a documented rejection of
the prototype. Check acyclic closure and Linux CSPA before accepting a change
that should generalize. A new routing framework is not a prerequisite.

## Reduce full-closure evaluation time

Use serial cyclic/acyclic closure at scale 4 as the targets. Closing the
measured gap would require about 32-37% less complete-command time; this is
the distance to the rival, not an unconditional implementation commitment.
Separate tuple work and comparison cost from growth, output and scheduling
before attributing time. A high inclusive insertion sample is not all hashing.

First finish the existing derived-relation tag decision on the working
reference. The earlier constructor-only experiment improves cyclic/chain
closure about 19%/14% serially, with 8/16 MiB extra final storage at scale 2.
Validate same-generation, wider-tuple and larger-storage controls, plus the
default import/startup path. Reuse the existing diagnostics and failure tests.
This may close part of the gap; it does not justify a new membership container.

Evaluate ThinLTO independently on a fixed engine/toolchain if its supported
build and installation constraints can be satisfied. Keep alignment separate,
and resolve the source-ownership audit and installed-path disclosure issues
in [open work](../TODO.md) before adopting a build default. Measure any accepted
tag/build combination directly; do not multiply their isolated improvements.

Profile the residual only after these decisions. A further change needs a
named hot operation and a bounded removal of work. Check sparse closure,
same-generation recursion and a representative analysis before accepting it.
Preserve the default path and report parallel CPU as well as wall time.

## Improve files and facts throughput

Start with `files-grouped` and `facts-join` at scale 8, one and sixteen workers.
Reuse import ownership and symbol-package isolation evidence. Fill the missing
attribution with a phase profile of the current complete commands: input and
value publication, expression evaluation, join/membership work, intermediate
materialization, folds and output. Compare executed work with the rival plan
where available. Direct phase evidence is required; subtracting independent
import/full medians does not produce a reliable evaluation timer.

Choose the first intervention from that attribution. The
[symbol-package comparison](../bench/compare/recorded/m3-max/import-isolation/README.md)
shows a real parallel benefit, but only about a 10-15% complete-command saving
in its own cohort and no consistent serial gain. It does not explain the
current 2-5 times gaps. Consider restoration or a smaller publication change
only for the work it demonstrably removes, with its maintenance cost explicit.
If expressions, repeated lookups or materialization dominate instead, target
that work. An aggregate proposal needs a smaller ownership boundary than the
withdrawn general reducer before it earns a prototype.

Retain ordinary fold/EMIT semantics, deterministic value numbering, input
limits, stream liveness and refusal behavior. Use an integer-only import,
small input and a non-grouping query as controls. Exit only on a useful
complete-command improvement with acceptable default latency and memory, or
close the candidate. A faster import microbenchmark alone is insufficient.
The scalar shortcut remains inconclusive, and import ownership evidence does
not justify a 120 MB target through small cleanup. Those investigations create
no further implementation work.

## Preserve gains and validate the composition

After selected changes, compare the combined engine with the initial reference
and relevant rivals under one output policy. Include the three target families,
a fast bound-recursion case, sparse/same-generation controls, Linux CSPA and
small commands. Add larger sizes or a second platform to answer a specific
scaling or portability question, rather than rerunning every old stage matrix.
Keep the older dense-analysis question explicit; a Linux CSPA win does not
settle it. Compiler policy stays fixed during engine comparisons.

Reuse the full normal/sanitizer validation of unchanged paths; run the required
checks for each actual change. Preserve correctness at plan/index refusal,
rewrite limits and reuse, import fallback and input limits. Report default
latency, parallel speed, peak/accounted memory and permanent maintenance cost
together. A memory/time tradeoff is acceptable only as an explicit decision.

Close completed work and publish one combined result. Keep narrower successes
without claiming they solve a larger gap. If one focused candidate is rejected,
move to the next justified priority; an inconclusive result does not authorize
a chain of heuristics, another container or another execution model.
