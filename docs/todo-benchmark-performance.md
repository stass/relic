# Performance plan

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`, `lib/exec.c`, `lib/import.c`, `bench/compare/`]

Reduce the remaining cost of useful commands while keeping one understandable
execution and ownership model. The next experiment is derived-relation
membership tags for full closure, followed by files/facts attribution. Parallel memory remains the largest
resource gap, with a design gate before further implementation. A candidate
belongs to a gap; it does not create another implementation track.

## Remaining gaps and execution order

The [rough rival comparison](../bench/compare/recorded/m3-max/rough-rivals/README.md)
identifies the large differences. Its two fixed-order runs rank questions;
they do not establish release performance or explain allocation ownership.
Times include loading, evaluation and output. Memory figures are process
peak RSS, in decimal units.

| Next work | Remaining gap | Evidence and decision |
| --- | --- | --- |
| 1 | Full-closure evaluation time | Default-thread cyclic/acyclic closure takes 37.0/9.92 s against 23.4/6.76 s: about 13.6/3.2 s excess at scale 4. This is a material default-path cost with an existing small membership candidate. |
| 2 | Files and facts throughput | At scale 8, grouped files is 2.9/5.3 times slower than DuckDB at one/sixteen threads; facts join is 1.9/2.6 times slower. The measured excess is 0.31-0.85 s and still needs phase attribution. |
| Design gate | Parallel working memory | At sixteen threads cyclic closure peaks at 8.47 GB against Souffle's 0.42 GB. Reusing worker screens lowers a matched comparison's peak to 6.66 GB but slows it 31%; that prototype is rejected. Actual worker candidate copies remain the dominant storage problem. |

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

The [scale-4 attribution and rejected screen-reuse prototype](../bench/compare/recorded/m3-max/merge-screen-reuse/README.md)
identify the remaining boundary. In the peak round at sixteen workers,
102.4 million worker copies represent 8.52 million new tuples. Candidate
capacity is 5.18 GB, with 3.89 GB occupied, and find scratch adds 2.57 GB.
Every worker is active. The sampled charged peak precedes the process's
largest sampled resident memory; those observations are not interchangeable.

Do not continue the screen-reuse prototype or add a policy to rescue it.
Earlier screen release improves charged admission but barely changes RSS;
it is not the next capacity experiment. The current evidence does not support
another reservation/lifetime tweak as a solution to the main gap.

Reopen implementation only for one focused design that reduces actual worker
copies or bounds their storage while preserving ordered publication,
deterministic value/tuple identifiers and ordinary serial retry. State the
expected bound through gathering, growth, merge and failure before coding.
The [parallel questions](todo-parallel.md) retain the relevant evidence and
constraints; they are not a scheduling feature queue.

Any such candidate must retain useful parallel speed on dense closure and a
large low-duplicate computed-value control. Check acyclic closure and Linux
CSPA before acceptance. A lower scratch charge alone, a peak shifted into
another phase or serial replay concealing failed admission is insufficient.
Until a concrete design meets this boundary, proceed with the closure-cost
experiment below.

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
