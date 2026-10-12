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
the import's time and peak the item on importing facts there. Record each
improvement on an idle machine with the same output sink and compiler
conditions, exact answer checks and peak memory. The
[M3 diagnostic evidence](bench/compare/recorded/m3-max/optimization-probes/results.json)
and [losing cells](bench/compare/recorded/m3-max/optimization-probes/losses.tsv)
also identify priorities in recursive planning, bound-query factoring,
bulk loading, [indexed driver slicing](specs/design-driver-slicing.md),
[direct grouped reduction](specs/design-group-reduction.md) and
[bounded candidate routing](specs/design-round-routing.md); the parallel work is in
[docs/todo-parallel.md](docs/todo-parallel.md).
[Canonical membership locality](specs/design-membership-locality.md)
defines the subsequent measured choice of tags or cached hashes, with
retention of the compact index when neither earns its cost. The
[implementation plan](docs/todo-benchmark-performance.md) orders these
changes and defines their design, regression and performance gates.

**Parallel evaluation misses its speed-up targets.** [Medium, component:
`lib/round.c`, `docs/todo-parallel.md`] With the pool, `tc-cyclic` should
run at least six times faster at `-j 16` than at `-j 1`, and with the
parallel merge at least ten times; it is 6.2-fold, 2.78 s to 0.45 s, the
fastest of five runs on a loaded macOS Xeon, which meets the first and
misses the second. `dist/3` and `object/2`, which compute integers and
strings, should scale as `tc-cyclic` does; `dist/3` scales 2.5-fold.
Components side by side, designed in `specs/design-parallel.md`, are not
built, and parallel loading misses its target. The causes and what to try
are sorted in `docs/todo-parallel.md`.

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
The [membership locality design](specs/design-membership-locality.md)
includes that cached-hash candidate alongside the compact reference and
tagged alternatives. Measure complete `chain-closure` runs and isolated
growth against steady storage and peak memory; successful duplicate
checks alone do not settle the tradeoff. Its
[adoption rule](specs/design-membership-locality.md#adoption-rule)
governs the shared experiment. Record measured rejection, if applicable,
in the design's alternatives with the benchmark artifact.

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

**A batch import's admission proof restates the scalar checks.** [Medium,
component: `lib/window.c`, `lib/import.c`, `lib/facts.c`, `lib/value.c`]
A window is admitted only if reading it a field at a time could not fail,
and the proof holds because each check is copied: `decode_field()` repeats
`field_value()`'s type checks and `relic_symbol_intern()`'s length
ceiling, `relic_pending_fits()` the integer ceiling of
`reserve_integers()`, and `relic_staging_admits()` the limits of
`admit_new_tuple()`, the builder's ceilings and the floor
`staging_set_of()` adds. A check added to the scalar path alone lets a
window admit what a field at a time refuses. The builder's column bytes
are one such check missing: `relic_staging_admits()` bounds the column
words, but on a 32-bit build without `max_memory` the next stepwise column
capacity can overflow its byte size, so a window is admitted and then its
reservation fails, keeping none of the values a field at a time would
have interned before the same failure. Give each layer one pure check that
both paths call, the builder's growth projected once for admission and
reservation, so the proof is the scalar check applied to the window's
totals.

**A batch import frames each window on the calling thread.** [Medium,
component: `lib/import.c`] Framing is one pass of the reader over the
window, and the pool waits for it. Sampled at eight threads, it is 38% of
the calling thread on a 158 MB table of two columns of repeated values
and 17% on 52 MB of distinct symbols, the largest serial step left. The
reader alone may decide where records end
([bulk loading](specs/design-bulk-load.md)), so the candidate is to frame
the next window, from the boundary the current window's framing found, on
a thread of the pool while the current window is analyzed, discarding it
when the current one is declined.

**A batch import places new values in the symbol index one at a time.**
[Medium, component: `lib/symbol.c`, `lib/value.c`] Publishing a window's
values is serial: on 52 MB of distinct symbols at eight threads it is 20%
of the calling thread, nearly all of it `relic_idtab_place()` under
`relic_symbol_append()`; the byte copies do not show in the samples.
Placing in parallel needs the symbol index partitioned by hash, as the
canonical index is by stripe, which is the storage-layout alternative the
bulk-load design names under persistent lookup partitioning.

**An import into a populated store appends its facts one at a time.**
[Medium, component: `lib/relation.c`, `lib/facts.c`] An empty store adopts
a builder's payload whole, but `append_builder()` copies, indexes and
counts each staged tuple on the calling thread when the store already
holds facts. The bulk-load design's append copies the columns, then places
each canonical stripe as a job of its own, publishing counts and totals
after the barrier; prepare those stripe lists before the commit's no-failure
phase.

**A batch window's scratch grows with its records, not its bytes.** [Low,
component: `lib/window.c`, `lib/import.c`] A window holds up to
`RELIC_IMPORT_WINDOW_BYTES` of input, but its tuple, hash and fresh arrays
take about 16 bytes per record, so a table of one-byte records takes some
32 MiB of scratch per 2 MiB window, outside the memory account. Cap the
records a window holds as well as its bytes.

**One declined window reads the rest of the table a field at a time.**
[Low, component: `lib/import.c`] A record longer than a window less a
read chunk, or one refused allocation, ends batching for the rest of the
import, which may be most of a large table. Resume windows once the
reader stands at a record boundary with its input consumed.

**An index built mid-component must be raised by its builder.** [Medium,
component: `lib/index.c`, `lib/relation.h`, `lib/eval.c`] Each index keeps
its own `latest_since`, yet `set_delta()` always raises every index of a
member to the same start, so an index built between rounds is correct only
because `prepare_candidate()` raises it; a build anywhere else would leave
its delta lookups walking from a stale frontier, which only an assertion
notices. Keep the start on the relation instead, which a new index's
frontiers, each at its key's first tuple, satisfy at any start; that
deletes the per-build raise, provided clearing a relation resets the start.

**A planner that emits an indexed operation must record its access.**
[Low, component: `lib/plan.c`] `place_atom()`, `place_probe()` and
`place_aggregate()` each call `record_access()`. An operation holds its
relation read-only, which `-Wcast-qual` keeps a single walk of the finished
draft from undoing, so the draft's accesses cannot be derived from its
operations. A missed call fails loudly, preparation reporting an internal
error, but undercharges acquisition until then.

## Benchmarks

**Validate a build policy against code-placement regressions.** [Medium,
component: `bench/compare/`, `configure.ac`, `tests/globals.sh`]
The [original cohorts](bench/compare/recorded/freebsd-5995wx/recursive-planning/report.txt)
measure 2.6-2.7% more serial cyclic-closure time and 1.3-1.4% more
facts-join time with equal tuple work and relation storage. The
[diagnostic controls](bench/compare/recorded/freebsd-5995wx/recursive-planning-analysis/report.txt)
reproduce the facts increase with import and validation alone. Permuting
only linked text sections changes the cyclic revision comparison from
+2.69% to -0.89% and facts-join from +1.70% to +0.02%; one permutation
does not establish a robust build or attribute every part of the cyclic
difference. The
[alignment and hardware-counter comparison](bench/compare/recorded/freebsd-5995wx/code-alignment/report.txt)
identifies 64-byte function alignment as a candidate: cyclic median spread
across three layouts falls from 2.67% to 0.83%, with 5.90% more text. The
[isolated probe-placement test](bench/compare/recorded/freebsd-5995wx/probe-alignment/report.txt)
does not support a probe-only attribute: offsets 0, 16 and 32 differ by at
most 0.20% on closure with surrounding code fixed. The C alignment
attribute slows closure by 1.37% in ordinary link order. The
[four-way build comparison](bench/compare/recorded/freebsd-5995wx/build-policy/report.txt)
finds ThinLTO lowers ordinary serial medians by 12.5-24.2% and selected
64-worker medians by 5.8-18.4%, with 29.6% more text. Adding global
64-byte alignment keeps the nine substantive serial layout spreads below
0.75%, but grows text by 35.1% and costs 1.1-1.5% on 64-worker cyclic
closure against ThinLTO alone in two cohorts. Validate ThinLTO across
supported machines and toolchains before setting a default; keep alignment
a separately measured choice.
Preserve equal argument lengths, multiple layouts, and guarded and
ordinary-Magic references. Repeat these controls after bulk loading and
indexed driver slicing; steps 4-5 of the benchmark plan cover those paths.

**Compare output sinks and validate scaling on a second platform.**
[Medium, component: `bench/compare/`]
Complete and audit the FreeBSD scale sweep, then compare the machine-specific
results with [the Mac report](BENCHMARKS.md). Separate OS/compiler/engine
version differences from core-count effects. Run the uniform stdout-output sweep and compare it with the preserved
file-output cohort to quantify output-sink costs, especially for large
answers. Keep their raw measurements and comparison tables distinct.

**Bound query factoring awaits its rival comparison.** [Medium, component:
`lib/magic.c`, `bench/compare/tc-cyclic-bf`] The M3 Max scale-8 matrix
records 189.242 s at one thread and 27.032 s at sixteen for the 8,000
answers of `tc-cyclic-bf`, which the unfactored rewrite reaches after
3,191,837,759 derivations. [Bound query factoring](specs/design-query-factoring.md)
answers it with 805,760 derivations in about 0.1 s in a local run. Measure
`chain-right`, `tc-acyclic-bf` and `tc-cyclic-bf` against the rival engines
under the [ordered plan](docs/todo-benchmark-performance.md#3-factor-eligible-bound-recursion).

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

An M3 Max scale-2 `tc-cyclic` evaluation-window profile places 1,739 of
2,542 samples in the insertion path; it is retained with the diagnostic
evidence. Batched hashing and prefetching are already present. Measure
probe lengths, tuple reads and cache locality before selecting a different
canonical index layout; a wider slot must justify its memory cost on
both dense closures and sparse analyses.

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
`lib/import.c`, `lib/symbol.c`, `lib/relation.c`,
`bench/compare/facts-join`] The target is `facts-join` at scale 10, three
million ternary rows naming three million distinct symbols, under 2 s and
120 MB. Bulk loading builds private payloads that an empty store adopts
and batches large tables over the pool; at scale 8 the import alone takes
0.89 s at one thread and 0.66 s at sixteen on the p620, and 0.34 s at
sixteen on the M3, against DuckDB's complete sixteen-thread median of
0.191 s ([step 4](docs/todo-benchmark-performance.md#4-build-and-load-base-relations-in-bulk)).
The serial steps left -- framing, symbol placement and the populated-store
append, the entries above -- bound the parallel gain. Measure scale 10,
time and peak, once they are parallel. Pool-entry packing remains a
separate memory-layout candidate and must preserve byte-pointer lifetime,
addressable range and affordable rehash.

## Testing and CI

**Native global merging hides source ownership from the object audit.**
[Medium, component: `tests/globals.sh`] On Apple clang at `-O3`, a mutable
file-static struct with separately used integer and pointer fields becomes
`__MergedGlobals` in the native object. Its C spelling is reserved to the
implementation, so the audit cannot distinguish it from compiler-owned
state. Preserve declaration provenance before native optimization rather
than special-casing generated names or disabling production optimizations.
The bitcode audit checks data-candidate names across lowering; native
objects need an equivalent source of ownership information.

**ThinLTO archives retain the compiler's source filename.** [Medium,
component: `m4/relic_cflags.m4`, `Makefile.am`] Clang's `-ffile-prefix-map`
rewrites debug paths but not LLVM IR's `source_filename`. An out-of-tree
build invoked through an absolute source path exposes that path in its
installed bitcode archive. Make source naming reproducible independently
of how configure is invoked, while preserving out-of-tree builds, compiler
wrappers and cross-compilation. Relative source paths avoid the disclosure;
the build instructions describe that packaging constraint.

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
  rounds, the parallel merge and parallel loading are built; components
  side by side, a threshold that weighs new tuples, streaming queries and
  the speed-up targets remain, in `docs/todo-parallel.md` and the major
  initiative above
* better cardinality estimation
* further Magic Sets and negation optimization
* floating point [Low, component: language], in `docs/todo-language.md`,
  specified in `specs/design-floats.md`; the item on type tests under
  Engine comes first
