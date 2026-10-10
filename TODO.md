# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Release relic 1.0.** [Critical, component: `specs/design-release.md`]
What remains, per the design's
[release gate](specs/design-release.md#the-release-gate): a green run of
`extended.yml`, its random-program and fuzz jobs included, and `macos.yml`
on the release commit, since every job of the matrix must pass on it; FreeBSD, OpenBSD and NetBSD, which no workflow
reaches, built and tested from the tarball by hand with no autotools
present, the result recorded in the release's `NEWS` entry; the packaging
skeletons the design's
[packaging skeletons](specs/design-release.md#packaging-skeletons) describe,
of which `contrib/` holds only the homebrew formula, so
`contrib/freebsd/devel/relic/`, `contrib/openbsd/devel/relic/` and
`contrib/pkgsrc/devel/relic/` are to write; and the
[release procedure](specs/design-release.md#the-release-procedure).

**Close the comparison gaps.** [Medium, component: `bench/compare/`,
`cmd/relic/output.c`] Six changes were made to close relic's gaps against
the other engines of `bench/compare/run.sh`, each with a target, and three
targets are missed. The row writer's share of `chain-closure`'s profile
is 11 per cent against a target under 5; what remains is formatting 24 MB
of rows, and no cheaper formatting is designed. The peak above the live
heap is the item on a growth's slack and transient under Benchmarks, and
the import's time and peak the item on importing facts there. The work is
done when the targets hold and the comparison is recorded on an idle
machine (the item on an idle run under Benchmarks).

**Parallel evaluation misses its speed-up targets.** [Medium, component:
`lib/round.c`, `docs/todo-parallel.md`] With the pool, `tc-cyclic` should
run at least six times faster at `-j 16` than at `-j 1`, and with the
parallel merge at least ten times; it is 6.2-fold, 2.78 s to 0.45 s, the
fastest of five runs on a loaded macOS Xeon, which meets the first and
misses the second. `dist/3` and `object/2`, which compute integers and
strings, should scale as `tc-cyclic` does; `dist/3` scales 2.5-fold.
Loading in parallel and components side by side, designed in
`specs/design-parallel.md`, are not built. The causes and what to try are
sorted in `docs/todo-parallel.md`.

## Engine

**A table's growth hashes every tuple again.** [Low, component:
`lib/util.c`, `lib/relation.c`] `relic_idtab_complete_growth()` rehashes
each entry through a callback that reads the tuple or key it names and
hashes it again, so a canonical index growing to two million tuples
recomputes some four million hashes from columns scattered through storage.
On serial `chain-closure` that is a quarter of the run,
`relic_hash_words()` under `relic_idtab_reserve()` in a sampled profile. It
is also most of what a deferred round's merge costs on the rounds that grow
the canonical index, which hold `chain-closure`'s average merge at 130
microseconds against 66 for a typical round. A slot that kept the 32-bit
hash beside the identifier would rehash from the table alone.
[The identifier table](specs/design-values.md#the-identifier-table) keeps
the identifier alone because, on the closure workloads' duplicate checks,
the doubled slot array costs about what the avoided reads save; the
growth's cost was not in that measurement. Measure the hash-keeping slot on
`chain-closure`'s growth against its peak memory before deciding, and if it
loses, add the growth to the reason given under
[alternatives not taken](specs/design-values.md#alternatives-not-taken) so
the question stays closed.

**Type tests read the type bits directly.** [Medium, component:
`lib/exec.c`, `lib/rule.c`] Floats take tag 3 with the top bit of the index
set, which works only if every type test goes through the accessors that
split an identifier into type and index
([the identifier](specs/design-floats.md#the-identifier)). Before floats
land, route these through a third-bit-aware accessor: `lib/exec.c` masks
`RELIC_DOM_TYPE_MASK` itself at 765 and 794 and in `cell_type()` at 816,
and in the min/max fold's mixed-type check at 1881, which would treat a
group mixing integers and floats as one type; `lib/rule.c` does at 840.

**A Magic rewrite found unstratified reports user errors first.** [Low,
component: `lib/eval.c`, `lib/stratify.c`] `evaluate_rewrite()` treats an
unstratified rewrite as an internal error, but `relic_schedule_build()`
has already run `report_cycle()`, so the user would read "not stratified"
diagnostics about generated predicates before the internal error. Build
the rewrite's schedule without reporting cycles.

**`lib/regex.h` shadows the system `<regex.h>`.** [Low, component:
`lib/regex.h`, `Makefile.am`] `AM_CPPFLAGS` puts `-I$(top_srcdir)/lib` on
the include path, so any system header that includes `<regex.h>` would get
relic's. Rename the header.

**Nothing bounds an input's size.** [Low, component: `lib/parse.c`,
`lib/import.c`, `include/relic.h`] A load reads its whole stream before
lexing (`read_stream()`), and an import grows one field's buffer without
bound, so an unterminated csv quote buffers the rest of the file. The
memory limit leaves both out by contract. A bound on either belongs in the
context's configuration, beside the other limits.

## Benchmarks

**Compare output sinks and validate scaling on a second platform.**
[Medium, component: `bench/compare/`]
Complete and audit the FreeBSD scale sweep, then compare the machine-specific
results with [the Mac report](BENCHMARKS.md). Separate OS/compiler/engine
version differences from core-count effects. Measure output-sink costs in
separate experiments: Souffle and Nemo export files while the other engines
write to discarded stdout. Compare Souffle's `-D -` and an equivalent Nemo
mode, preserving the existing workflow measurements as a distinct cohort.

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
was measured without batching, median user time of five interleaved runs
on the macOS Xeon: `tc-cyclic` 4.75 s with four-byte slots against 4.36 s
with eight, `sg-cyclic` 3.42 against 3.56 s, at peaks of 81-82 against
90-116 MB and 56-69 against 77-102 MB, so it was set aside. Worth measuring
next: the stored hash again with batched insertion in place, and whether a
successful search visits enough slots past the first to make prefetching
them pay.

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

**Importing facts misses its target.** [Medium, component:
`lib/symbol.c`, `lib/value.c`, `lib/facts.c`, `lib/import.c`,
`bench/compare/facts-join`] The target is `facts-join` at scale 10, three
million ternary rows naming three million distinct symbols, under 2 s and
120 MB; on the macOS Xeon it takes 2.9 to 3.2 s and about 220 MiB of
resident peak. The peak is the import's
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

## Testing and CI

**LeakSanitizer flags macOS's fork handler.** [Low, component:
`tests/random/main.c`, `Makefile.am`] Built with Homebrew's LLVM, where
AddressSanitizer detects leaks on macOS, every child the random driver forks
reports memory `_notify_fork_child` in libSystem allocates, and the child's
exit status makes the driver report a failure and shrink a program that does
nothing wrong. Linux, where CI runs the driver, is unaffected. A LeakSanitizer
suppression for that frame, passed by `check-random` and `check-asan`, would
let a macOS developer run them with leak detection.

## Post-1.0

Candidate features, roughly in priority order: what remains of the
language features first, since they decide which programs can be written;
then incremental maintenance and parallel evaluation, which change the
engine and cost the most; floating point last, since no workload measured
needs it. None may change the language semantics of v1.

**Language and tooling gaps.** [Medium] What Souffle, LogiQL, Nemo, clingo,
CodeQL, DDlog and the tabling Prologs offer that relic does not, sorted in
`docs/todo-language.md` in the order worth adding them -- provenance
first, then disjunction, file inclusion, constraints, per-rule profiling
and floating point; what stays out on purpose is in
[non-goals](specs/architecture.md#non-goals). Each item names the engines
that have it and what a relic program does without it; the verdicts are
provisional until a design exists.

* string operations, specified in
  [functions](specs/language.md#functions) and
  `specs/design-expressions.md`, are built; what remains:
  * [Medium, performance, `lib/exec.c`, `lib/value.c`] the serial cost is
    above its target. Over a million sources, `object/2`, `object(M, O)
    :- source(M, S), O = cat(substr(S, 0, length(S) - 2), ".o")`, adds
    0.60 s to the load where the same join assigning a constant adds
    0.13 s, 4.6 times against a target of twice. Each computed string is
    probed in the pool twice, when its text slot is bound and again by the
    measure, which is most of the 0.32 s `str(M)` over the same sources
    costs; interning a million new strings is most of the rest. Probing
    only for a slot some later operation reads as a key, and not for one
    only an `EMIT` or an expression reads, would halve the probes without
    changing what the measure decides; it is a planner decision that
    changes [slots and cells](specs/design-expressions.md#slots-and-cells),
    which probes at every assignment.
  * [Medium, performance, `lib/merge.c`] `object/2` should scale as
    `tc-cyclic` does at sixteen threads: over a million sources it
    evaluates in 0.60 s past the load on one thread and 0.33 s on sixteen,
    1.8 times, where `tc-cyclic` scales 5.3 times (2.66 s to 0.50 s). At
    sixteen threads the tasks take 0.059 s, gathering the candidates
    0.025 s, applying them 0.070 s, and the measure of the pending strings
    0.163 s, serially: it probes the pool for each gathered string and
    numbers the new ones in order of first occurrence. Striping the measure
    by the string's hash, as the apply is striped by the tuple's, would run
    the probes in parallel and leave only the numbering serial, over the
    new strings alone; that changes
    [interning computed values](specs/design-values.md#interning-computed-values),
    which specifies one serial measure.
* incremental insertion maintenance [Medium, component: `lib/eval.c`]: any
  change to the program after an evaluation discards every derived relation
  and computes the fixed point again from the facts, so an embedder or a
  session that loads a large program and adds to it a little at a time pays
  for the whole program on every addition. Continuing from the tuples held
  is sound without negation, where a new base fact is a delta the
  semi-naive machinery already understands, and unsound with it, where a new
  fact can retract a derived one;
  [when the relations are current](specs/design-eval.md#when-the-relations-are-current)
  settles one rule for both, and [non-goals](specs/architecture.md#non-goals)
  defers maintenance. Measure how often a caller adds facts between queries
  before building either half.
* DRed-style deletion maintenance
* parallel evaluation, specified in `specs/design-parallel.md`: shared
  rounds and the parallel merge are built; loading in parallel, components
  side by side, a threshold that weighs new tuples, streaming queries and
  the speed-up targets remain, in `docs/todo-parallel.md` and the major
  initiative above
* better cardinality estimation
* further Magic Sets and negation optimization
* floating point [Low, component: language], in `docs/todo-language.md`,
  specified in `specs/design-floats.md`; the item on type tests under
  Engine comes first
