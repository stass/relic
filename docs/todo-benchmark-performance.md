# Performance plan

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`,
`lib/relation.c`, `lib/exec.c`, `lib/import.c`, `bench/compare/`]

Reduce the remaining cost of useful commands while keeping one understandable
execution and ownership model. Derived-relation hash tags and inline probe
helpers are part of the working reference, with native compilation. Next
compare that combination directly with the initial control and relevant
rivals.
Parallel memory remains the largest resource gap, with a design gate before
further implementation. A candidate belongs to a gap; it does not create
another implementation track.

## Remaining gaps and execution order

The [rough rival comparison](../bench/compare/recorded/m3-max/rough-rivals/README.md)
identifies the large differences. Its two fixed-order runs rank questions;
they do not establish release performance or explain allocation ownership.
Times include loading, evaluation and output. Memory figures are process
peak RSS, in decimal units.

| Next work | Remaining gap | Evidence and decision |
| --- | --- | --- |
| 1 | Composition and remaining gaps | Compare the accepted native engine directly with the original control and relevant rivals. Inline probes bring serial cyclic/acyclic scale-4 medians to 25.45/8.06 s in their own cohort; the older rival times are 23.4/6.76 s. Establish the combined changes and remaining gaps under one output policy. |
| Design gate | Files and facts throughput | The rough refresh puts grouped files 2.9/5.3 times behind DuckDB at one/sixteen threads and facts join 1.9/2.6 times behind. Profiles identify input, expressions and materialization as the main costs. A literal-prefix prototype does not establish a useful files gain; larger interventions need a focused ownership and reuse design. |
| Design gate | Parallel working memory | At sixteen threads cyclic closure peaks at 8.47 GB against Souffle's 0.42 GB. Reusing worker screens lowers a matched comparison's peak to 6.66 GB but slows it 31%; that prototype is rejected. Actual worker candidate copies remain the dominant storage problem. |

Eligible bound-recursion losses have reversed. Keep those gains as regression
controls. Near ties and the roughly 2 ms serial chain-left gap do not justify
new execution machinery. The [bounded Andersen/nonlinear refresh](../bench/compare/recorded/m3-max/dense-refresh/README.md)
also favors relic's time at scale 8, one and sixteen workers. Keep those
cases as controls; their higher memory use reinforces the existing memory
question without creating another time-optimization track.

## Reference and decision gates

The initial control is the scan-only engine measured in the
[trim comparison](../bench/compare/recorded/scan-only.md) and rival refresh.
Keep its frozen source, builds, fixtures and answers. The working reference
uses derived-relation tags and inline probe helpers with native compilation.
The [tag](../bench/compare/recorded/m3-max/derived-tags/README.md) and
[inlining](../bench/compare/recorded/m3-max/probe-inline/README.md) comparisons
retain exact answers, controls and resource costs. Use the inlining cohort's
selected binary for the composition comparison.
Measure the final combination directly against the initial control; separate
cohorts' improvements do not multiply into a measured combined result.

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
Until a concrete design meets this boundary, proceed with composition
validation.

## Hold the closure and build decisions fixed

Use serial cyclic/acyclic closure at scale 4 as the targets, with the
selected tags enabled. Retain the same-generation, sparse, wider-tuple,
large-storage, import/startup and Linux CSPA controls from the adoption
cohort. Its measurements do not select another canonical container or
attribute all remaining insertion time to hashing.

Keep native compilation as the default and comparison baseline. The
[optional ThinLTO comparison](../bench/compare/recorded/m3-max/thinlto/README.md)
measures the tagged engine directly. ThinLTO stays explicit because its
installed static archive requires a compatible LLVM consumer toolchain;
building another library solely to change that default adds an artifact and
test matrix. Report optional-build gains separately from default-engine
improvements. The source-ownership audit and installed-path issues remain
separate correctness work in [open work](../TODO.md).

Keep the selected probe helpers and table representation fixed during the
composition comparison, which determines the
remaining closure gap. Another closure intervention needs new attribution
and one bounded removal of work; the earlier helper profiles do not select
another container or a sequence of speculative inlining changes.

## Require a focused design for remaining throughput

The [complete-command CPU profiles](../bench/compare/recorded/m3-max/files-facts-profile/README.md)
attribute facts mainly to input: 70-77% of sampled CPU, with secondary-index
construction at 7-13%. Files splits between input (42-44%), expressions
(25-35%) and intermediate materialization (11-18%); the final fold accounts
for under 2%. These are sampled CPU shares, not elapsed phase timers or
predicted speedups. The rival plans expose different work, including earlier
facts filtering and direct files grouping, but do not choose a new ownership
model for relic.

The [literal-prefix screen](../bench/compare/recorded/m3-max/regex-prefix-screen/README.md)
does not establish a useful complete-files gain, so its prototype is not
adopted. Its large synthetic near-match improvement does not select another
optimization track. Keep the current matcher for composition validation.

Further throughput work needs another specific removal of measured work with
an explicit ownership and reuse contract. Replacing a first lookup with a
scan does not prove its index is avoidable or single-use. The
[symbol-package comparison](../bench/compare/recorded/m3-max/import-isolation/README.md)
shows a real parallel benefit, but only about a 10-15% complete-command saving
in its own cohort and no consistent serial gain. It does not explain the
current 2-5 times gaps. A publication or aggregate proposal must earn its
maintenance cost through the work it removes and a smaller ownership boundary
than the withdrawn general reducer.

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
Reuse the bounded Andersen/nonlinear comparison for the unchanged paths;
it does not settle every larger scale. Compiler policy stays fixed during
engine comparisons.

Use a bounded descriptive comparison with the already validated immutable
binaries: two alternating pairs against the initial engine and the existing
two-run rival protocol. Keep exact independent answers for the engine pair
and normalized-set checks for rivals. This reports the combined tradeoff and
remaining large gaps; it is not another adoption decision or a cross-host
release claim. Do not infer small regressions from the rough rival ordering.

Reuse the full normal/sanitizer validation of unchanged paths; run the required
checks for each actual change. Preserve correctness at plan/index refusal,
rewrite limits and reuse, import fallback and input limits. Report default
latency, parallel speed, peak/accounted memory and permanent maintenance cost
together. A memory/time tradeoff is acceptable only as an explicit decision.

Close completed work and publish one combined result. Keep narrower successes
without claiming they solve a larger gap. If one focused candidate is rejected,
move to the next justified priority; an inconclusive result does not authorize
a chain of heuristics, another container or another execution model.
