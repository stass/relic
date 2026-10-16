# Performance work

[Medium, components: `lib/eval.c`, `lib/import.c`, `lib/facts.c`,
`lib/relation.c`, `bench/compare/`]

Use the accepted dense-collection engine as the native control. Its
[recorded comparison](../bench/compare/recorded/dense-rounds/README.md) includes
the missed 90% RSS aim and the remaining roughly twofold memory gap on the fresh
FreeBSD rival comparison. The
[earlier performance pass](../bench/compare/recorded/performance-pass.md)
retains the other rival gaps and rejected approaches.

## Decide the serial membership cache

The [serial investigation](../bench/compare/recorded/serial-seen/README.md)
identifies repeated insertion checks as a material operation. Its isolated
142-line positive-cache prototype improves cyclic closure from 25.68 s to
13.30 s and acyclic closure from 8.07 s to 5.12 s in a two-pair M3 screen.
It requests a 2 MB bitmap on those cells; measured RSS rises by about 7 MB
and 4 MB. The small controls do not establish absence of regression.

Write the production design around the same single-plan ownership boundary
before bringing runtime code into the tree. Keep the roughly 180-line runtime
budget. A bit is learned only from completed ordinary insertion; a miss uses
the existing batch inserter. Keep typed coordinates, out-of-span fallback,
original refusal-prefix accounting, and the configured-memory-limit decline.
Do not add persistent relation state, cache invalidation hooks, a worker
protocol or a public tuning switch.

Freeze a candidate and use the [measurement protocol](todo-performance-measurements.md)
for ten alternating pairs of the two closure targets and the sparse, bound
and computed controls. Resolve the small bound/computed losses before
adoption; a persistent unexplained default-path loss blocks it. Keep the
observed RSS cost visible. Add a supported-host comparison and a fresh serial
Souffle comparison with identical output policy before claiming the rival
gap closed. Preserve full answers, order, identifiers, counters, final
accounted storage and refusal behavior; prove successful cache use separately
from timing. Run the required correctness and sanitizer checks on the final
implementation, including an unchanged parallel composition control.

Keep or reject this bounded candidate from that evidence. Retain every
observation and failed attempt; do not grow a sequence of tuning variants.

## Choose the following measured gap

After the serial decision, use the resulting engine as the control for
files/facts throughput. Choose the representative cell by absolute excess
time and bytes, then attribute parsing, value assignment, relation/index
work or executor work before proposing a change. Further dense-memory work
requires attribution of the common append and storage, which outlive the
collector arrays.

Each new candidate needs one measured owner or operation, the mechanism it
removes, a source budget, controls and a rejection criterion. Begin with a
small fixed screen; only a useful result warrants decisive measurement.
Measure accepted changes together and freeze a new reference. Close an
unproductive candidate; keep unresolved toolchain validation work in the
testing category of `TODO.md`.
