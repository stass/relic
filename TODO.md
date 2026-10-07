# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Release relic 1.0.** [Critical, component: `specs/design-release.md`]
What remains: a green run of `extended.yml` and `macos.yml` on the release
commit, which architecture section 68 requires of every job; FreeBSD built
and tested from the tarball by hand; and the release procedure of the
design's section 5.

**Close the comparison gaps.** [Medium, component:
`specs/design-comparison-cost.md`] The design's section 10 gates each of its
six changes on a measurement, and three gates are missed. Step 1: the row
writer is 11 per cent of `chain-closure` against the 5 per cent asked; what
remains is formatting 24 MB of rows, and no cheaper formatting is designed.
Step 5: the item on a growth's slack and transient under Benchmarks. Step 6:
the item on importing facts under Benchmarks. The work is done when the
gates hold and the comparison is recorded on an idle machine (the item on
an idle run under Benchmarks).

**Parallel evaluation misses its speed-up gates.** [Medium, component:
`lib/round.c`, `docs/todo-parallel.md`] `specs/design-parallel.md` section
15 gates step 3 on a six-fold and step 4 on a ten-fold speed-up of
`tc-cyclic` at sixteen threads; it is 5.3-fold. `dist/3` scales 2.7-fold,
missing the step 4 gate of `specs/design-arithmetic.md` as well. Steps 5
and 6 are not started. The causes and what to try are sorted in
`docs/todo-parallel.md`.

## Engine

**A deferred round costs more than a serial one on one thread.** [Medium,
component: `lib/task.c`, `lib/merge.c`, `specs/design-deferral-cost.md`]
Under `-X parallel` at `-j 1`, `tc-cyclic` costs a fifth more than
serially, `cspa` a twelfth and `chain-closure` half as much again, with
identical counters. The design's section 2 measured where: the whole of
`tc-cyclic`'s loss is the one round that inserts a third of what it
derives, whose task phase takes twice the serial round and whose merge
half as long again; the merge costs 100 to 150 ns per new tuple on every
workload. The floor is proportional to new tuples, not derivations or
rounds. Steps 1 to 4 of the design's section 10 take it down and then
re-measure the threshold of `specs/design-parallel.md` section 5.4. Step
1, the screen probed in passes and the fast path, is in: round 1 of
`tc-cyclic`'s task phase at 0.86 of its time before. Step 2, buffers
reserved from what each item inserted last, is in: the screen moves
nothing on a round with a history, and `chain-closure`'s task phase is
at 0.83 of step 1's. Step 3, the merge's lists and blocks, is in: a
typical round of `chain-closure` merges in 66 microseconds from 99, and
the mean round in 130 from 160, held up by the few rounds that grow the
canonical index, which the serial path grows as well. Step 4 is not. The
round that costs most, a recursive rule's first, has no history and
keeps its growth cascade.

**A table's growth hashes every tuple again.** [Low, component:
`lib/util.c`, `lib/relation.c`] `relic_idtab_complete_growth()` rehashes
each entry through a callback that reads the tuple or key it names and
hashes it again, so a canonical index growing to two million tuples
recomputes some four million hashes from columns scattered through
storage. On serial `chain-closure` that is a quarter of the run,
`relic_hash_words()` under `relic_idtab_reserve()` in a sampled profile.
A slot that kept the 32-bit hash beside the identifier would rehash from
the table alone. `specs/design-memory.md` section 6 measured that slot on
the closure workloads' duplicate checks, which gain nothing from it, and
kept the identifier alone for the memory; the growth's cost was not in
that measurement. Measure the hash-keeping slot on `chain-closure`'s
growth against its peak memory before deciding, and if it loses, record
the figure in `specs/design-memory.md` section 6 so the question stays
closed.

## Benchmarks

**`bench/compare/run.sh` compares on one thread only.** [Low, component:
`bench/compare/run.sh`] The harness runs every engine on one thread, Souffle
with `-j1` and DuckDB with `threads = 1`, so nothing records how relic's
speed-up at `-j 16` compares. A threaded column needs a Souffle built with
OpenMP, since the one on this machine's `PATH` is not and ignores `-j`, and
its programs translated with `-j16`, since a compiled Souffle program has
parallel loops only when translated with `-j`. One comparison made by hand
with Souffle 2.5 built with OpenMP had compiled Souffle leading at sixteen
threads on `tc-cyclic` and relic leading on `cspa` and `chain-closure`.

**No run is recorded on an idle machine.** [Medium, component: `bench/`]
`specs/design-bench.md` section 8 keeps one comparison run in
`bench/compare/results.txt`, which does not exist: it needs an idle
machine, ten timed runs per cell and the scale sweep, `BENCH_SCALE` 1, 2, 4
and 8, so the curves show which gaps grow with the input. Section 8's
criteria also ask for a second machine of a different operating system and
processor; every run so far is one macOS Xeon. `bench/baseline.txt` was
recorded on a machine busy with other work; its counters are exact, but its
seconds are longer than an idle run's and want recording again in the same
session. The same session should settle whether Souffle and Nemo, which
write their answers to files where the other engines write to a discarded
standard output, pay for it: under load a Souffle run writing its file
showed wall time far above its CPU time, within the noise of a busy
machine. If the file costs a measurable amount, time Souffle with `-D -`
and find Nemo an equivalent. The run should include the Graspan workloads,
`-w cspa-httpd,cspa-postgresql,cspa-linux`, which a default run leaves out.
Single runs of each, one thread on a quiet machine, gave relic 210, 190 and
50 seconds against compiled Souffle's 171, 195 and 54, at peaks of 6.2, 6.6
and 2.1 GB against 5.5, 7.2 and 1.3.

**A bound right-recursive closure costs relic more than the whole
closure.** [Medium, component: `lib/magic.c`, `bench/compare/tc-cyclic-bf`]
On the dense cyclic graph, `tc(500, X)?` over `tc(X, Z) :- par(X, Y),
tc(Y, Z)` took relic 28 s against 17 s for `tc(X, Y)?`, measured on a busy
machine. The Magic rewrite carries the binding through the recursive call to
every node reached, which here is every node, so it computes the whole
closure and pays for the magic relation besides. The rewrite is correct but
the query's answer is only the nodes reachable from 500. `bench/run.sh`
shows the same on a chain: the right-recursive bound query derives two
million tuples for an answer of 2000, the left-recursive one six thousand.
A right-linear recursion bound on its first argument can be answered by
reachability over `par` alone (the right-linear and factoring rewrites of
Naughton, Ramakrishnan, Sagiv and Ullman); find whether relic's rewrite can
recognize the case. Souffle's `--magic-transform` produces the identical
program and runs it in about 0.9 of relic's time, so the gap to Souffle is
in evaluation rather than in the rewrite: see the next item.

**relic's evaluation costs several times Souffle's interpreter per derivation
on the analyses.** [Medium, component: `lib/eval.c`, `bench/compare/andersen`]
On a busy machine `andersen` took relic 1.9 s against 0.7 s for the Souffle
interpreter, and the gap widens on denser inputs: a denser
`dataset_andersen` took 248 s of user time against 17 s, and `tc-nonlinear`
over `sparse-cyclic` ran past 600 s against 117 s. Semi-naive evaluation
does not repeat work here, so the gap is the cost of a derivation and its
lookups, not their number. Profiles of `tc-cyclic` and `sg-cyclic` put 59
and 49 per cent of the time in the duplicate check of relation insertion;
92 to 96 per cent of those checks find a duplicate, each a cache miss on
the slot array and another on the tuple that confirms it. Souffle's B-tree,
inserted with the outer loop fixed on the head's first column, lands
consecutive inserts in the same leaf. A hash stored beside each identifier
was measured and set aside (`specs/design-evaluation-cost.md` section 5.2).
Worth measuring next: the stored hash again with batched insertion in
place, and whether a successful search visits enough slots past the first
to make prefetching them pay.

**A growth's slack and transient keep the peak above the live heap.**
[Medium, component: `lib/util.c`] `chain-closure`'s peak footprint runs
from 34 to 49 MiB and its resident peak from 44 to 59 MiB, against a live
heap of 33.6 MB. An identifier table's capacity is a power of two because a probe
masks the hash, and a column array doubles because one policy serves every
arity, so a relation just past a doubling is half empty on both. A table's
growth holds both generations of its slots while it rehashes, one and a
half times the new size at that moment, and the allocator keeps the freed
generation for reuse. Growth by three halves under a multiply-shift range
reduction, `(hash * capacity) >> 32`, would keep a mask-free probe and
halve the slack. Measure it on `chain-closure` and `facts-join`, by peak
footprint as well as resident set, before changing the policy. The memory
limit does not count the transient either: an insertion or a round's merge
admitted at the limit holds one array's or one table's old generation
beside the new one while it is copied, one at a time.

**Importing facts misses the comparison's gate.** [Medium, component:
`lib/symbol.c`, `lib/value.c`, `lib/facts.c`, `lib/import.c`,
`bench/compare/facts-join`] `specs/design-comparison-cost.md` section 10
gates its step 6 on `facts-join` at scale 10, three million ternary rows
naming three million distinct symbols, under 2 s and 120 MB; it takes 2.9
to 3.2 s and about 220 MiB of resident peak. The peak is the import's
commit, which holds at once the staged facts (about 68 MiB), the symbols
(about 95 MiB, of which 46 MiB is sixteen-byte entries) and the base store
it appends to (50 MiB). Two thirds of the time is the import, mostly
interning the symbols and the staging set, each probe a cache miss into a
table of millions; the query is the rest, much of it building the join's
index. Candidates, none designed: for a load into an empty store, handing
the staged facts to the store rather than copying them, so that the peak
holds them once; an entry of a 32-bit offset into the string arena and a
length, eight bytes rather than sixteen, about 23 MiB; growing the pool by
walking the entries in identifier order and placing each by its stored
hash; and checking an imported symbol field once, where the import checks
it is a symbol and interning the value checks again.

## Post-1.0

Candidate features, in the priority order of `specs/architecture.md` section
69. None may change the language semantics of v1.

**Language and tooling gaps.** [Medium] What Souffle, LogiQL, Nemo, clingo,
CodeQL, DDlog and the tabling Prologs offer that relic does not, sorted in
`docs/todo-language.md` in the order worth adding them -- provenance
first, then disjunction, file inclusion, constraints, relation export,
per-rule profiling and floating point; what stays out on purpose is in
architecture section 3. Each item names the engines that have it and what a relic
program does without it; the verdicts are provisional until a design
exists.

* string operations, specified in `specs/design-strings.md`, are built;
  what remains:
  * [Medium, performance, `lib/exec.c`, `lib/value.c`] the step-2 gate is
    not met: over a million sources, `object/2` of the design's section 2
    adds 0.60 s to the load where the same join assigning a constant adds
    0.13 s, 4.6 times against the 2 the gate asks. Each computed string is
    probed in the pool twice, when its text slot is bound and again by the
    measure, which is most of the 0.32 s `str(M)` over the same sources
    costs; interning a million new strings is most of the rest. Probing
    only for a slot some later operation reads as a key, and not for one
    only an `EMIT` or an expression reads, would halve the probes without
    changing what the measure decides; it is a planner decision the
    design's section 8.2 would record.
  * [Medium, performance, `lib/merge.c`] the step-4 gate is not met: over
    a million sources, `object/2` evaluates in 0.60 s past the load on one
    thread and 0.33 s on sixteen, 1.8 times, where `tc-cyclic` scales 5.3
    times (2.66 s to 0.50 s). At sixteen threads the tasks take 0.059 s,
    gathering the candidates 0.025 s, applying them 0.070 s, and the
    measure of the pending strings 0.163 s, serially: it probes the pool
    for each gathered string and numbers the new ones in order of first
    occurrence. Striping the measure by the string's hash, as the apply is
    striped by the tuple's, would run the probes in parallel and leave only
    the numbering serial, over the new strings alone; that changes the
    design's section 9.2, which specifies one serial measure.
* relation export, in `docs/todo-language.md`
* incremental insertion maintenance [Medium, component: `lib/eval.c`]: any
  change to the program after an evaluation discards every derived relation
  and computes the fixed point again from the facts, so an embedder or a
  session that loads a large program and adds to it a little at a time pays
  for the whole program on every addition. Continuing from the tuples held
  is sound without negation, where a new base fact is a delta the
  semi-naive machinery already understands, and unsound with it, where a new
  fact can retract a derived one; `specs/design-eval.md` section 6 settles
  one rule for both, and architecture section 70 defers maintenance.
  Measure how often a caller adds facts between queries before building
  either half.
* DRed-style deletion maintenance
* parallel SCC evaluation, specified in `specs/design-parallel.md`: steps 1
  to 4 of its section 15 (the executor scratch, deferred rounds on one
  thread, the pool and the parallel merge) are built; loading in parallel
  and components side by side remain, and the speed-up gates are a major
  initiative above
* better cardinality estimation
* further Magic Sets and negation optimization
* floating point [Low, component: language], in `docs/todo-language.md`,
  specified in `specs/design-floats.md`
