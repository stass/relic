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

## Build and packaging

**Continuous integration is incomplete.** [High, component:
`.github/workflows/`] `specs/architecture.md` section 68 specifies fourteen
jobs.
`ci.yml` has the Linux compilers, AddressSanitizer with leak detection and
UndefinedBehaviorSanitizer, distcheck, and the tarball built on glibc and
musl without autotools; `extended.yml`, weekly, has MemorySanitizer,
ThreadSanitizer, `-m32`, no threads and the two distcheck variants;
`macos.yml` runs only by hand while the repository is private. `ci.yml`
has passed on GitHub; `extended.yml` and `macos.yml` each need a first
green run before they can be trusted. Missing:

* FreeBSD, OpenBSD and NetBSD. Success criterion 10 of architecture section 72
  asks for each. Release 1.0 is excepted: it is built and tested from its
  tarball on FreeBSD by hand, and OpenBSD and NetBSD, with this job, are 1.1
  work. GitHub has no BSD runners, and the vmactions actions that boot one on
  a Linux runner fetch their helper script and disk images at run time with no
  checksum, which was judged too much trust to extend. A job that boots each
  system's official image, verified against the checksum the project
  publishes, under QEMU on a Linux runner, or a hosted service with native BSD
  machines, would close it.
* macOS on every push, once minutes on it are free.

**`contrib/` has no BSD port skeletons.** [Low, component: `contrib/`,
`specs/design-release.md` section 4] Only the Homebrew formula exists. The
FreeBSD and OpenBSD ports and the pkgsrc package the design lists are 1.1
work, with those systems' testing; each fetches the published tarball, runs
`make check` where the system has a test target, and lists the installed paths
exactly.

**No file over 2 GiB has been loaded on a 32-bit Linux system.** [Low,
component: `lib/parse.c` `relic_open_input`] Such a file is opened with
`O_LARGEFILE` and read through stdio, which glibc and musl track with 64-bit
offsets internally; nothing has run it. A 32-bit build loading a sparse file
of facts past 2 GiB would confirm it.

## Engine

**Striped tables multiply the number of mappings.** [Low, component:
`lib/util.c`] Every stripe's arrays are mapped once they reach
`RELIC_ARRAY_MAP_BYTES`, so a large program holds several times the
mappings it did with unstriped tables. On Linux the default
`vm.max_map_count` of 65530 would be reached with some 8 GB of arrays just
past the 128 KiB threshold, rather than 64 GB at a mebibyte. Unverified;
worth measuring on Linux before a release that promises tens of gigabytes.

**The discard's mark is set where a relation is handed out, not derived.**
[Low, component: `lib/relation.c`] `holds_derived` is set by the write
funnel and by an index build, and the discard returns at once when it is
clear, which keeps a one-fact `relic_fact_add()` from walking every
predicate. That is right because nothing writes a derived relation through
a pointer handed out before a discard, which holds today since every
evaluation discards before it plans and nothing commits while one runs; a
future path that kept such a pointer would leave derived tuples the next
discard skips, and a query would read them as current. The discard empties
the derived relations but keeps them, promising that a pointer to one still
names the same relation, and that is why whether anything is derived has to
be remembered beside them. Free them instead, and the indexes of the base
stores with them, creating each again on first use as it is created now:
something is derived exactly when a derived relation or a base store's
index exists, a count kept where they are created and freed makes the
discard's early return a structural fact, and a write through a stale
pointer becomes a use after free the sanitizers report. First find what
relies on a derived relation's pointer outliving a discard.

**An interactive session accepts only queries.** [Low, component:
`cmd/relic/repl.c`, `include/relic.h`] A session cannot add a fact or a rule,
because a load that fails its check leaves the program rejected for good, and
the tool cannot check text without loading it. Accepting statements needs a
load that is checked as one step and undone when the check fails; that is an
added entry point, a compatible change, and the session would then load a
line ending in `.` through it. A session would then pay for re-evaluating
the whole program after each addition (incremental insertion, under
Post-1.0) and for the complete relations evaluated again for every query.

**A long-lived context's tables only grow.** [Low, component:
`lib/parse.c`, `lib/magic.c`, `lib/symbol.c`, `lib/value.c`,
`cmd/relic/repl.c`] No table releases an entry, and two kinds of entry
outlive the query that made them. A query's text: a constant is interned as
a value, a variable's spelling and the query's source name as symbols, so
an embedder preparing a query per request over distinct constants grows the
symbol pool or the integer table by one entry per distinct constant, and a
session grows the symbol table by one entry per line, because the tool
names each line's query `<query N>`. A rewrite's generated names: every
adorned and magic predicate, and the symbol naming it, stays after the
query; the same query reuses them, so the growth is bounded by the
distinct adornments ever asked for. Nothing walks these tables per answer,
so the cost is memory, bounded by the distinct text and query shapes seen.
A fix is a source name that is not interned (diagnostics copy it) and a
per-query scope for the symbols, values and generated predicates only one
query uses; that is a change to interning and wants a design note first.

**The end of typed input may be taken for a refused line.** [Medium,
component: `cmd/relic/input.c`] `read_edited()` takes a NULL from
`el_gets()` with `errno` EILSEQ for a line libedit could not read in the
locale's encoding. A libedit that discards a byte that is not text, as the
NetBSD one on glibc does, may leave EILSEQ in `errno` from that discard and
return NULL for a Ctrl-D typed after it, which relic would report as a
refused line, needing a second Ctrl-D to end the session. Read from the
source, not seen on macOS's libedit, which ends the read itself; confirm on
Linux, and if so, tell the two apart by what else `el_gets()` returns.

**An aggregate over one atom copies the atom's relation.** [Low,
component: `lib/rule.c`, `lib/eval.c`, `specs/design-aggregates.md`] Each
aggregate folds a group predicate its group rule fills, the cost of storing
the body's relation once that `specs/design-aggregates.md` section 5.1
accepts, so
`N = count { valueAlias(A, B) }` first copies every `valueAlias` tuple into
`agg$$q0$1(A, B)`, with its index. On Graspan's httpd, on 16 threads, a
query counting the three CSPA relations this way peaks at 30 GB, where
answering `valueFlow` over the same fixed point peaks at 7.8 GB.
A group rule whose body is one positive atom of distinct variables, its
head those variables, holds exactly the atom's relation, so the aggregate
could fold that relation through an index on its shared columns and the
group predicate need not exist.

**A query can derive more through the rewrite than without it.** [Medium,
component: `lib/magic.c`] Each adornment of a predicate derives a relation of
its own, and the budget bounds the rules generated, not the tuples those
relations hold. Once any call reaches a predicate with nothing bound, every
other call of it reads that free relation, so `path(X, Y)?` holds `path`
once. A query can still reach one predicate with several bound patterns and
none free, whether or not the query binds a column itself: a ternary
predicate called with its first column bound and with its second derives
two relations, each of which may approach the whole of it. The general
remedy is for an adornment to read one that binds a subset of its columns; a
cap on adornments per predicate, falling back to the free one, would bound
the multiple. The rewrite is correct as it stands, and the tuples count
against `max_tuples` and `max_memory`.

**The adornment search and the draft each walk a body.** [Medium, component:
`lib/magic.c`] The search for the adornments a query reaches
(`reach_calls_in_order()`) and the draft of the rewritten rules
(`rewrite_body()`) each plan a rule's body and walk it, and they must agree
on the rules walked, the order, the columns each call binds and which calls
read on demand. The only guard is the internal-error diagnostic
`adornment_of_call()` gives for a call the search never recorded, so an edit
to one walker would surface as a failed query on some program rather than as
a failing test. Recording, per adornment and rule the search walks, the
order and the calls it reached would let the draft replay the record by
construction, and let the second search reuse the first's plans rather than
plan each body again.

The same record would let the Magic limits count what the rewrite keeps
rather than what it explores. Each search is held to `magic-preds`,
`magic-rules` and the literal budget on its own, and the rule limit counts
every rule a search reads where the draft keeps fewer, dropping duplicates,
rules that derive nothing and bound adornments a later free call
subsumes. So a configured `magic-rules` can refuse a query whose finished
rewrite would have fit (`tests/lang/magic-rules-read.dl`).

**Reading a query's reach completely is a mode beside the rewrite.** [Low,
component: `lib/magic.c`, `lib/eval.c`, `lib/explain.c`] When no adornment
the rewrite keeps binds a column, it generates nothing and sets
`reads_completely`, which evaluation and explain each test, and the
rewrite's query then aliases the caller's rule where it is otherwise an
arena copy. `read_reached_completely()` closes the reached components over
their dependencies with a closure of its own. Treating the reached
predicates as complete components from the start, as the negated part of a
query already is, would remove both branches and the second closure.

**Coverage gaps.** [Low, component: `tests/unit/test_exhaustion.c`]
`make coverage` reaches 97 percent of the library's and the tool's lines;
the rest are defensive branches kept deliberately and the allocation
failures the exhaustion suite cannot reach. The suite refuses the
allocator's requests, and through the arena fault point every arena request
of a program loaded, checked and answered both through the Magic rewrite
and from the whole fixed point. What neither reaches is an intern that
fails only when it lands on a table's growth point: in `lib/parse.c`,
`parse_literal` and the `advance` of `collect_body`, for a load's constant
or its interning of `not` or `_`; in `lib/magic.c`,
`relic_magic_find_complete` interning a base view; and in `lib/rule.c`,
`compile_head` interning a head's constant. A context filled to one entry
short of a growth before the operation would reach them.

**A caller tells a limit's refusal from another failure only by its
text.** [Low, component: `include/relic.h`, `lib/diag.c`] Every limit is
reported through `relic_diag_limit()` with the prefix `limit exceeded:`, and
the eval fuzz target tells a limit's refusal from a failure by that prefix.
An error class a caller can ask for would free both from the wording.

**A deferred round costs more than a serial one on one thread.** [Medium,
component: `lib/task.c`, `lib/round.c`] Under `-X parallel`, in one build
and with identical counters, `tc-cyclic` takes 2.62 s against 2.24 s,
`cspa` 0.21 s against 0.19 s, and `chain-closure` 0.61 s against 0.42 s.
Screening a batch probes the store as inserting it does, and a tuple the
store lacks then pays for the worker's screen and candidate buffer and is
probed once more in the merge; the screen starts empty every round and is
rehashed at each doubling. That overhead is the floor under every gain the
pool can give, and it is what the threshold of
`specs/design-parallel.md` section 5.4 must be measured against. Reserving
the screen from what the round's items estimate, rather than growing it
from nothing, is the first thing to try.

**The pool's spin bound counts iterations, not time.** [Low, component:
`lib/pool.c`] `RELIC_SPIN_LIMIT` pauses 4096 times before a waiting worker
parks, which was sized on x86, where `pause` takes tens to a hundred
cycles; `yield` on aarch64 is close to free, so a worker there parks
within microseconds and a component of many small rounds pays a wake-up
per round. A bound in time, the clock read every few hundred pauses,
would hold on every processor. Unmeasured on aarch64.

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
[Medium, component: `lib/util.c`] Mapped growth releases every outgrown
generation, but `chain-closure` still peaks at 39.4 MB against a live heap
of 33.6 MB, 17 per cent over the 10 per cent that
`specs/design-comparison-cost.md` section 10 gates step 5 on. Two causes
remain. An identifier table's capacity is a power of two because a probe
masks the hash, and a column array doubles because one policy serves every
arity, so a relation just past a doubling is half empty on both. And a
table's growth holds both generations of its slots while it rehashes, one
and a half times the new size at that moment. Growth by three halves under
a multiply-shift range reduction, `(hash * capacity) >> 32`, would keep a
mask-free probe and halve the slack; rehashing a mapped table within its
own extended mapping would remove the transient where the platform can
extend one. Measure both on `chain-closure` and `facts-join` before
changing either policy. The memory limit does not count the transient
either: an insertion or a round's merge admitted at the limit holds one
array's or one table's old generation beside the new one while it is
copied, one at a time.

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

## Security residuals

**Mapped arrays are outside AddressSanitizer's quarantine and leak check.**
[Low, component: `lib/util.c`] A growable array of `RELIC_ARRAY_MAP_BYTES`,
128 KiB, or more lives in a mapping of its own. The sanitizer reports a
write just outside it, since the header before the first item and the
bytes past the last are marked unaddressable, but a stale pointer into a
mapping already released reads whatever is mapped there next instead of
being reported as a use after free, and a mapping never released is not
reported as a leak. The
evaluator holds no pointer into a relation across a growth, which is what
such a stale pointer would need. A sanitizer build that kept every array in
the heap would catch both, at the cost of not exercising the mapped path
under the sanitizer.

## Post-1.0

Candidate features, in the priority order of `specs/architecture.md` section
69. None may change the language semantics of v1.

**Language and tooling gaps.** [Medium] What Souffle, LogiQL, Nemo, clingo,
CodeQL, DDlog and the tabling Prologs offer that relic does not, sorted in
`docs/todo-language.md` into what is worth adding -- provenance first,
then disjunction, file inclusion, constraints, relation export and
per-rule profiling -- what is worth measuring before deciding, and what
stays out on purpose with the reason. Each item names the engines that
have it and what a relic program does without it; the verdicts are
provisional until a design exists.

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
* persistent compiled rule plans
* better cardinality estimation
* further Magic Sets and negation optimization
