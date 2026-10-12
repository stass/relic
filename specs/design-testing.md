# Testing

How relic establishes that its answers are right and that untrusted input
cannot break it: an oracle kept inside the library, a set of equivalences
every optimization is held to, a generator of random programs that are valid
by construction, a differential driver and a shrinker, fuzz targets over the
loader, the evaluator, the importer and the regex matcher, coverage, the
sanitizer builds, and the mutation check that shows the rest can fail. All
of it is a client of the public interface, `<relic.h>`, or of the tool; none
of it changes the language. It is engineering around the system, with one
exception that lives inside it: the oracle.

Related: [build](architecture.md#build),
[determinism](architecture.md#determinism), [safety](language.md#safety),
[stratification](language.md#stratification),
[evaluation](design-eval.md), [library interface](design-api.md),
[benchmarks](design-bench.md), [release](design-release.md).

## Model

Three kinds of evidence back an answer, each covering what the others
cannot. Hand-written expectations -- the unit tests and the golden language
suite -- are the only check on code every strategy shares (see [What no
oracle covers](#what-no-oracle-covers)). Agreement between strategies finds
a bug in one optimization by comparing it with another; the curated corpus,
the random generator and the eval fuzz target all feed that comparison.
Fuzzing and the sanitizer builds look for crashes, hangs, leaks and
undefined behaviour rather than wrong answers.

Every equivalence is reachable through the ordinary interface: the
`RELIC_F_` configuration flags in `include/relic.h` and their `-X` spellings
in `relic(1)`. The suites need no private entry point into the library and
test what an embedder gets; that is why `-X` exists at all (see
[principles](architecture.md#principles) and
[command line](architecture.md#command-line)).

## Invariants

* A strategy may change the cost of an answer, never the answer. Flags never
  change answers; a disagreement is a bug, whichever side is wrong.
* A generated program is safe, stratified, within every structural limit,
  defined (it meets no evaluation error) and terminating, all by
  construction. So any refusal, evaluation error or hang on a generated
  program is itself a finding.
* A test that can pass without checking anything is a defect. Each harness
  either proves it can fail or anchors itself to a hand-written expectation.
* A reproducer that once mattered is replayed by every build, on every
  compiler and sanitizer configuration, forever.

## The oracle

The naive fixed point lives in the library, in `lib/eval.c`, selected by
`RELIC_F_NAIVE` (`-X naive`). It runs every rule of a component over
everything until a full pass inserts nothing new: a short loop over the same
per-component pass the semi-naive evaluator uses. It stops only when every
rule, run over everything, adds nothing, which is the definition of the least
fixed point, so its correctness needs no argument about deltas, views or
rounds.

Keeping the oracle inside the library, rather than writing a reference
implementation in the test suite, means both evaluators share the parser, the
program model, the relation store, the planner and the executor. A
differential failure between them then isolates the fixed-point logic instead
of implicating the whole pipeline. A separate reference implementation would
duplicate the front end and test the duplicate. The cost is a small amount of
production code that exists for testing; it is held to the same review and
sanitizer requirements as everything else and stays for the life of the
project.

The strongest reference is the naive fixed point over the whole program,
`RELIC_F_NAIVE | RELIC_F_NO_MAGIC`: no deltas, no rewrite, every relation
complete. The eval fuzz target and the derivation-count oracle both use it.

## Equivalences under test

| Equivalence | Selected by |
| --- | --- |
| semi-naive == naive | `-X naive` |
| Magic rewrite == whole-program evaluation | `-X no-magic` |
| factored bound recursion == ordinary Magic | `-X no-factor` |
| indexed == unindexed | `-X no-index` |
| planner order == textual order | `-X no-reorder` |
| existence checks == full generators | `-X no-exists` |
| deferred rounds == serial rounds, at any thread count | `-X parallel`, `-j N` |
| aggregates folded in place == group rules | `-X group-rules` |
| direct group reductions and summaries == materialized row folds | `-X no-group-reduction` |
| streamed rows == gathered answer | `-X no-stream` |

Naive, no-index, no-reorder and no-exists are also run without the rewrite, by
both `strategies.sh` and the random driver, so each is compared on the
program's own rules as well as on the rewritten ones; deferred rounds are
combined with no-magic by the random driver only, and streaming with no-magic
by `streaming.sh` only. Two further equivalences are reached only by the random
driver: a query over relations that already hold the fixed point
(`relic_materialize()` first) against the same query alone, and `_` under `not`
against its helper rendering (see [The differential
driver](#the-differential-driver)).

Three harnesses check them:

* `tests/equiv/strategies.sh` runs every registered language test under each
  strategy and compares sorted standard output, the diagnostics on standard
  error and the exit status with the default run. Group rules are held to
  output and status alone: they hold copies the default does not, which a
  limit message counts, and an unstratified program may be reported with a
  different cycle.
* `tests/equiv/streaming.sh` runs every registered test streaming and
  gathered, with and without the rewrite, and compares the rows unsorted,
  byte for byte: a plan that derives each row once delivers rows in
  derivation order, and the answer relation lists tuples in insertion order,
  which is the same order. The suite fails unless some run inserted fewer
  tuples streaming than gathering, since a corpus in which nothing streamed
  would compare a behaviour with itself.
* The random driver and the eval fuzz target, below.

Group-reduction tests also compare default and forced-deferred builds of
the same physical plan across worker counts. Curated cases pin unrequested
overflow, invalid groups, empty defaults, bound-result comparisons and
post-query materialization. The whole-program derivation oracle disables
group reduction so its expected row materialization and fold counts stay
independent; optimized runs compare answers and the appropriate tuple-total
bounds to that reference. Structural counters verify removed stores,
reused summaries and bounded partial workspace rather than relying on
elapsed time ([validation](design-group-reduction.md#validation)).

### Exclusions and anchoring

`strategies.sh` leaves out a test that chooses a strategy itself (`-X` in
its arguments), shows plans with `-e`, limits the rewrite, or is an
interactive session: each is about what the alternatives change. A test
with a `.per-strategy` file beside it is left out with the reason written
there, such as a program on which one strategy meets an undefined
arithmetic result another never evaluates, which the language allows (see
[evaluation errors](language.md#evaluation-errors)); a golden test per
strategy pins each behaviour. The diagnostics that report where evaluation
first met something -- the rewrite-outgrew-its-limit warning and the `-vv`
notes on mixed types -- are filtered, since that place legitimately differs
between strategies; the answers they describe are compared like any other.

Two runs that went wrong the same way agree. So the default run is also
held to the test's golden output wherever the test sorts its rows itself,
and the suite fails unless at least one run was anchored that way.

### Tuple-total reference pairs

Answers do not see a relation no query reads. Tuple totals do: after each
query, `-X stats` (or `relic_stats()`) reports how many tuples all relations
together hold, and a strategy that evaluates the same rules as its reference
must hold the same number.

Equal totals stand for equal relations because of two premises. The naive
fixed point is complete, so its total is the size of the least model of the
rules it evaluates. And every other strategy inserts only tuples those rules
justify: a delta is a range of its relation, so every match a delta variant
finds, the rule's full plan would find too. Every tuple a strategy holds is
then in the least model, and holding as many as the model has, it holds all
of them. The number of string values must match as well, since a computed
string is interned only as a column of a stored tuple.

The pairs are chosen by which rules a strategy evaluates:

* naive, no-index, no-exists, no-stream and parallel (at one and at several
  threads) evaluate the default's rules and are held to its total;
* the variants without the rewrite all evaluate the program's own rules and
  are held to the no-magic run's total;
* no-reorder under the rewrite has no reference: the planner's order is the
  rewrite's sideways information passing strategy, so changing it generates
  different rules, and only its answers are compared;
* no-factor generates the ordinary rules for adornments the default
  factors, so it is compared with the default on answers and diagnostics,
  and is the reference naive+no-factor's totals are held to;
* the group-rule strategies hold group relations the others fold away, so
  only the materialized strategy is held to group-rules without the rewrite;
  group-rules under the rewrite has no reference.

### The derivation-count oracle

Neither answers nor totals see a derivation made twice, since a duplicate
changes neither. The semi-naive evaluator promises every derivation exactly
once (see [evaluation](design-eval.md)), and the random driver checks it on the
materialized strategy. The expected count is one derivation per distinct fact
stated for a predicate that also has rules, plus one per distinct binding of
each rule body's variables over the fixed point, plus one per distinct binding
of each group body's. The bindings are counted in a separate context under the
naive fixed point without the rewrite, as the rows of one query per body.

The materialized strategy runs with group rules, so every aggregate body
derives and the oracle need not know which bodies the compiler folds in
place. Two shapes are not counted, because the expected count is not a
function of bindings: a positive literal holding `_`, where one binding may
match several tuples, and a positive literal holding a variable its rule
mentions nowhere else, which derives once per tuple or once per binding
depending on whether the planner makes it a generator or an existence check.
`_` under `not` multiplies nothing, since a negated literal lets a binding
through at most once.

## The random program generator

The generator lives in `tests/random/`. It builds a program model --
predicates, facts, rules and queries as small arrays of terms -- and prints
it as text.

**Programs, not strings.** A syntax fuzzer spends nearly all its time being
rejected by the lexer; the generator spends all of its time in the evaluator,
where the equivalences live. Malformed input is the load fuzz target's job.
The model is the generator's own, not `lib/ast.h`: a generator sharing the
library's representation would share its mistakes, and a term built wrongly
by both would agree.

**Determinism.** A program is a pure function of one seed. The generator
draws from its own splitmix64 stream, defined by its arithmetic alone, never
`rand(3)` or `random(3)`, whose sequences differ between C libraries; a seed
reported on one platform names the same program on every other. Every size
and every choice is drawn from the stream, so the corpus varies its shape as
well as its contents. The per-context hash seed is irrelevant: answers are
compared as sorted renderings and relations by tuple count, neither of which
depends on where a key sits in a table.

### Stratified by construction

Every predicate gets a level when it is created: facts-only predicates level
zero, derived predicates a level from one to three. A rule for a predicate at
level `h` may read

* positively, any predicate at a level `<= h`;
* negatively, any predicate at a level `< h`;
* inside an aggregate's braces, only predicates at levels `< h`.

Following a dependency from head to body, the level never rises, and along a
negative or aggregate edge it strictly falls. A cycle returns to its starting
level, so it cannot contain a negative or aggregate edge. Every generated
program is stratified by this argument alone; the generator has no
stratification check. Recursion and mutual recursion come from positive
literals at the head's own level, deep strata from negated ones.

A derived predicate's first rule reads only levels strictly below its own.
Without such a base rule, a predicate whose every rule reads itself holds
nothing, and a corpus of mostly empty relations agrees under every strategy
without testing any of them. For the same reason the proportions favour
satisfiable bodies: few shared variables, few constants in positive literals,
and a body seldom has a negation or a comparison and never more than two of
each.

### Safe by construction

A body is drawn in a fixed order. The positive relational literals come
first, from the rule's variables, constants and `_`. The set `P` of variables
they mention is then fixed, and every later choice -- the head's arguments,
the negated literals' arguments, comparison operands -- draws from `P` only.
That order is the [safety](language.md#safety) condition, so an unsafe rule
cannot be drawn. `_` appears only in positive and negated literals: a
positive literal matches anything there, and a negated one leaves the column
out of its probe. A comparison never holds `_`.

An assignment binds a variable outside `P` from variables in `P`, and is
drawn before anything reads that variable, which then joins `P`. The text may
place it anywhere in the body, since the library binds by the safety closure
and not by written order. An aggregate follows the same discipline inside
its braces: its own variables are fresh, a subset of `P` is shared with it
and each shared variable is placed in a positive literal of the group body,
so the group rule is safe and every shared slot is bound. Its result joins
`P` and is read by the head or a later comparison.

Arity is at most four and a rule mentions few variables, far inside every
structural [limit](architecture.md#limits), so no limit can refuse a
generated program.

### Defined and terminating by construction

A seed first decides whether its program computes. Only a program that
computes draws operators, sums and string calls, and it states only small
integers; a program that does not compute states the extremes of `int64_t`
among its integers and computes nothing. A program that computes must
neither meet an evaluation error nor derive values forever, and the
generator guarantees both without running anything.

* **Arithmetic.** Stated integers are small, a divisor is a nonzero
  constant, and a multiplication has a constant operand, so an expression of
  the shapes drawn is at most a fixed small multiple of its largest operand.
  Whether `X = expr` assigns or filters depends on what is bound where it
  runs, and the Magic rewrite runs a prefix of a rule with only the call's
  bindings, so any literal holding an operator may compute a new value.
  Operator literals are therefore drawn only in queries and in rules whose
  positive literals all read levels below the head's, at most two per body.
  A computed value then never feeds its own computation: bottom up, a value
  at level `h` comes from values below `h`; top down, the rewrite carries
  demand only from a higher level to a lower one. A value passes through a
  bounded number of bodies, each multiplying it by a bounded factor, so
  every value stays far below 2^63 and there are finitely many under every
  strategy.
* **Aggregates.** `min` and `max` pass on a value of the group. A `sum` is
  drawn only over group bodies that read facts alone, so every summand is a
  small stated integer and the group size is bounded by the facts. A `count`
  is the size of a derived relation; this one bound is not by construction,
  but it would take a relation far larger than anything a generated program
  derives within the harness's alarm, and if it were ever reached the
  overflow would fail the query visibly rather than hide a disagreement.
* **Strings.** The string functions count as operators and are drawn under
  the same budget. A pattern comes from a fixed list of valid ones, never
  from a variable, so no pattern fails to compile. String arguments are
  bound variables or short constants, so the longest string any program can
  build stays far below the length limit. An argument of the wrong type
  makes a call have no value, which filters the binding under every strategy
  alike.

Deleting a literal or a column, as the shrinker does, keeps every one of
these properties.

### What is varied

Arity from zero to four; self-, mutual and multiply recursive rules;
negation across levels, with and without `_`; a small value pool of
symbols, strings (the empty string, bytes that need escaping) and integers
(zero, negatives, both extremes of `int64_t`), so joins match and columns
mix types; all six comparison operators across types, exercising the
cross-type [comparison](language.md#comparisons) rules; predicates with both
facts and rules, which the rewrite reads on demand; and queries that are an
atom under any binding pattern, repeated variables and fully bound atoms
included, or a conjunctive body.

## The differential driver

Each program is loaded and checked once per strategy, each in a context of
its own created with `relic_create_config()`, and each query is collected
with `relic_query_collect()`. The strategies and their tuple-total references
are the table in `tests/random/compare.c`; they cover every equivalence
above, each combined with and without the rewrite except streaming, whose
pairing with no-magic `streaming.sh` covers, plus the materialized strategy
and the helper rendering.

**The helper rendering.** A program whose negated literals hold `_` is also
loaded with each such literal rewritten to negate a helper relation of its
named columns, derived by a positive literal holding the `_`. Its answers
under the default strategy must equal the program's. That holds the probe of
a negated literal to the meaning of `_` under `not` by a path that never
takes the probe, so a mistake every strategy shares is still found. Totals
are not compared, since the helper relations hold tuples the program does
not.

**Answers are compared as text.** A value's identifier belongs to the context
that interned it, so rows from two contexts cannot be compared as
`relic_value_t`. Each row is rendered -- a symbol as its spelling, a string
quoted with every byte outside printable ASCII escaped, an integer in decimal
-- and an answer's rendered rows are sorted bytewise. Equal sorted renderings
are set equality, which is what a query means.

**A refusal is a failure.** The driver installs a diagnostic callback. A
generated program the library refuses means the generator's construction and
the library's analyses disagree, and one of them is wrong. Warnings, such as
a singleton variable, are expected and ignored.

### Fork isolation

The engine under test may crash, hang, leak or trip a sanitizer, and each is
a finding rather than a reason for the harness to stop. Each program is
therefore evaluated in a child created with `fork(2)`:

* the child announces over a pipe which strategy it is about to run, and
  once it has one, its verdict;
* an alarm in the child turns a hang into a failure of its own kind;
* a child killed by a signal is a crash, charged to the strategy it last
  announced;
* a child that has sent its verdict exits through `exit(3)`, so a
  LeakSanitizer build checks it for leaks; a nonzero status, or an exit with
  no verdict, is abnormal whatever the verdict said.

Attribution holds by construction: the parent learns which strategy failed
from the order of the child's own announcements, made before each strategy
runs, not from anything a dying child has to manage to say. The exit status
carries only success or failure, so no status a sanitizer or the C runtime
chooses can be mistaken for a verdict.

### Failure kinds

A failure has a kind and, where one applies, the strategy that disagreed. The
kinds are: the library refused the program; a strategy's answers differ from
the default's; a tuple total differs from its reference's; a query failed with
no limit configured; the materialized run made a derivation twice or missed
one; the helper rendering answered otherwise (charged to no strategy, since all
agreed); an evaluation failed on an undefined result (overflow, zero divisor,
an over-long string, an invalid pattern), told apart from other query failures
by the error's class, not its text; the child crashed (recorded with its
signal), hung, or exited abnormally (recorded with its status), which is how a
sanitizer that reports and exits, rather than aborting, shows up. The
enumeration is in `tests/random/random.h`.

A report prints the seed, the kind, the strategy, what differed and the
shrunk program as text `relic` runs as it stands; with `-o DIR` the driver
also writes the original and the shrunk program there.

## The shrinker

A generated counterexample is a curiosity; its five-statement reduction is a
bug report. The shrinker reduces a failing program greedily. A pass tries
every deletion the program allows, largest first -- each query (while one
remains), each rule or fact, each body literal, each literal inside an
aggregate's braces, each argument position of a predicate everywhere it is
used -- and keeps a deletion whenever the smaller program still fails. Passes
repeat until one keeps nothing, so no single deletion of the result leaves
the failure in place.

**The failure is held fixed.** A deletion is kept only when the candidate
fails with the same kind, under the same strategy and, for a crash or
abnormal exit, with the same signal or status. That is what stops a
reduction sliding from one bug to an easier, unrelated one, or to a
candidate that fails only because a deletion made it invalid. Two kinds are
held more loosely, by necessity: a refused program is held by kind alone,
since the first strategy to load it always refuses it, so its reduction may
move between reasons for refusal; and an evaluation error is held by kind
and strategy, and may move between the four undefined results. Signal
distinction is only as fine as the signals: under a sanitizer built to
abort, every report ends in the same signal.

**Candidates stay valid.** Deletions add no dependency edge and no operator,
so stratification and the arithmetic bound survive. Removing an argument
position keeps every level and sign. A deletion that leaves a variable
unbound or a group body empty is caught by the model's own safety check and
never run.

Every candidate runs in a child exactly as the driver runs a program, so a
crash or a hang shrinks as readily as a wrong answer.

## The harness tests itself

A differential harness that cannot fail passes forever. The driver's
fault-injection mode (`-F`) drops one row from one comparison's answers to
any query with a row: the naive strategy's, compared with the default's, and
the helper rendering's, compared with the program's.
`tests/random/selftest.sh`, run by `make check`, requires each to be
reported as its own kind and shrunk to a handful of statements; the smallest
program with a row to drop is a fact and a query. It is the one test that
shows generation, the child, each comparison, the reported status and the
shrinker are all connected.

## The mutation check

The self-test shows a disagreement is reported; it does not show that the
engine's real mistakes produce one. The mutation check does, and it is
standing practice rather than a one-time exercise: whenever a comparison or
oracle is added, or a subsystem's core loop is reworked, plant plausible
bugs in a copy of the engine and run the suites over it. Typical plants are
a delta scan that skips its first tuple, a component member whose gains go
unrecorded, an index lookup that stops after one tuple, a supplementary
Magic predicate that drops a variable, or a fold that misreads a column. Each
must be found within a modest number of seeds or by the curated suites, and
shrink to a few statements. A plant that survives marks a gap: close it with
a test or record it in `TODO.md`. The plants go in a separate copy of the
tree, never the tree under development, so a forgotten plant cannot be
committed.

## What no oracle covers

A differential test finds a bug in one strategy by comparing it with
another, so a bug in something all of them use -- the value order,
interning, the parser, rendering of output, the comparison operators --
makes every strategy agree on the same wrong answer. The curated suites,
whose expectations are written by hand, cover that code:

* `tests/unit/` holds one C program per module, with plain assertions.
* `tests/lang/` holds the golden language suite: each `.dl` file is a test,
  run by `tests/langtest.sh`, comparing standard output, standard error and
  exit status with files beside it. `tests/langfiles.sh` fails when a
  program in the directory is not registered, so no test is silently
  unrun.
* `tests/lang/LANGUAGE` names the tests whose expectation the language alone
  fixes -- a failure's status and each query's rows as a set -- so another
  implementation can run them as a conformance suite; `langfiles.sh` holds
  every test that runs a program alone to being listed there or omitted with
  a reason.
* Structural checks hold properties no answer reveals: no mutable global in
  the library (`tests/globals.sh`, over the built objects); every allocator
  the library calls is one the allocation-failure suite can refuse
  (`tests/allocators.sh`, failing closed on an unknown C library function);
  the exported symbols match the header (`tests/symbols.sh`); the manual
  pages list the header's exports and the tool's options and pass
  `mandoc -Tlint` (`tests/man.sh`); no source byte is outside printable ASCII
  (`tests/ascii.sh`); every documented example runs (`tests/docs.sh`, see
  [release](design-release.md)); and performance stays proportional to size
  on counters, not the clock (`tests/perf.sh`).

The object audits use the configured compiler and symbol reader. Each LLVM
bitcode object is lowered to a private native object without further
optimization before its sections or undefined names are inspected. Every
named data candidate must survive lowering, and every object read must
succeed. The native format determines symbol spelling independently for
each object. `tests/object-audits.sh` exercises native and bitcode constants,
mutable globals, thread-local state, allocator references, paths with spaces
and corrupt objects, including corruption after a valid object.

Three things are deliberately absent from the differential runs:

* **No limits.** Strategies derive different amounts, so a limit would fire
  in one and not another and the comparison would be of two refusals.
  Limits are exercised by the golden suite and the eval fuzz target.
* **No comparison of diagnostics in the random driver.** Every strategy
  loads the same text, so warnings are identical by construction; the one
  that differs, the abandoned rewrite's warning, differs by design.
  `strategies.sh` does compare diagnostics over the curated corpus.
* **No syntax mutation.** That is the load fuzz target's job.

Every test runs through `tests/strict.sh`. A test that lacks an optional
tool (mandoc, nm, pkg-config) exits 77 and is counted as skipped; with
`RELIC_STRICT_CHECK=yes`, a skip not named in `RELIC_ALLOWED_SKIPS` is a
failure, so a machine set up to run every check cannot report a check that
ran nothing as passed.

## Regression categories

The curated suite keeps tests in each of these categories, and a fixed bug
adds its regression test to the category it belongs to: lexing and parsing;
malformed syntax and error recovery; variable safety; duplicate facts;
recursion, mutual recursion, cyclic graphs and rules with several recursive
literals; component ordering; stratified negation and rejected negative
recursion; comparisons over mixed-type columns; arithmetic and its
evaluation errors; string functions and regular expressions; aggregates;
every query binding pattern; Magic adornment, recursion and the
equivalence of a query with whole-program evaluation; import and export;
output formats; resource limits; interruption; parallel rounds at several
thread counts; allocation failure; and diagnostics, checked by golden
standard error.

## Fuzzing

### Four targets

Each is a libFuzzer entry point, `LLVMFuzzerTestOneInput()`, in
`tests/fuzz/`.

* **load** takes the input as program text, loads it and checks it. It
  covers the lexer, parser, program model, safety and stratification: the
  components most exposed to input nobody chose.
* **eval** goes on to answer every query three times: as a caller would by
  default, with every round deferred (`RELIC_F_PARALLEL_ALWAYS`), and with
  the oracle, `RELIC_F_NAIVE | RELIC_F_NO_MAGIC`. Where two answer a query,
  the answers must be the same set; a difference aborts. The three contexts
  must also accept or refuse the program alike. The default context installs
  an explain callback that discards every line, so the plan renderer runs
  over every program the fuzzer builds.
* **import** takes the input's first byte as a choice of format, header and
  column list from a small fixed table, and imports the rest through
  `relic_import_file()`, via a stream whose read function is the target's
  own where the platform supports one. It covers the table reader and the
  importer, and aborts when an import fails without reporting an error.
* **regex** feeds the matcher behind `match` (`lib/regex.c`) directly
  through its internal interface: the first byte is the pattern's length,
  the pattern follows, the string is the rest, NUL and every metacharacter
  allowed. A pattern must compile within its instruction bound or be refused
  with a failure inside the pattern, and a compiled pattern must match or
  not, reporting a start within the string, without a crash, a hang or a
  read outside its inputs.

### What counts as a finding

Inputs are bounded twice: `-max_len` bounds the text, and the eval target
sets tuple and memory limits so a program deriving a large relation is
refused rather than timing out. A query refused by a limit, or stopped by an
evaluation error, is excused: the oracle derives different amounts and
evaluates expressions over bindings the optimized strategy never reaches, so
one may stop where the other does not (see [evaluation
errors](language.md#evaluation-errors)), and only queries both answer are
compared. Deferral changes nothing but cost, so the deferred context must
answer exactly the queries the default one does. A query that fails for any
other reason, or fails without reporting an error, is a finding. The error
count is reset before each call, so a failure is never excused by an error
an earlier call left behind.

No limit counts steps, so a small relation with a large join -- a cross
product whose tuples are all duplicates -- stays inside every limit and can
still run long. `-timeout` bounds that, and a timeout on such an input needs
reading before it is called a bug.

### Replayed by every build

Each target is linked twice. Always, it is linked with a replay driver whose
`main()` passes each named file, or each file of a named directory, to the
entry point once; `tests/fuzz/replay.sh`, part of `make check`, replays every
corpus through its targets. So every input that ever mattered runs on every
compiler and under every sanitizer target, including platforms with no
libFuzzer. With `configure --enable-fuzzers`, each target is also linked
with `-fsanitize=fuzzer`.

### Corpora and dictionaries

There are three corpora, one per kind of input:

* **Programs**, read by load and eval: the language tests, which already
  hold every construct and malformed input the golden suite specifies, plus
  `tests/fuzz/corpus/`, which holds inputs the golden suite does not --
  raw bytes the lexer must refuse, programs the rewrite cuts into
  supplementary predicates, generated programs -- and every finding once its
  bug is fixed.
* **Tables**, read by import: `tests/fuzz/corpus-import/`, tables in both
  formats, with and without a header, under each column list, well formed
  and holding each kind of mistake.
* **Patterns and strings**, read by regex: `tests/fuzz/corpus-regex/`, the
  constructs of the dialect, the ways a pattern fails to compile, and
  patterns exponential for a backtracking matcher.

A reproducer is added to its corpus with a name saying what it found, and is
never deleted.

There are two dictionaries, holding tokens a mutation would otherwise have
to find byte by byte: `tests/fuzz/relic.dict`, the language's punctuation,
keywords, aggregate and function names, used by load and eval; and
`tests/fuzz/regex.dict`, the pattern dialect's metacharacters, used by regex.
The import target takes neither, since its input is a table.

### Running the fuzzers

`configure --enable-fuzzers` checks that the compiler links a program with
`-fsanitize=fuzzer` and fails if it cannot; a flag that quietly built
nothing would be worse. It then compiles the whole tree with the coverage
instrumentation libFuzzer steers by and links only the targets with
libFuzzer's `main()`. Apple's clang has no libFuzzer; LLVM's clang does. The
fuzzing build combines with `--enable-asan` and `--enable-ubsan` as the
developer chooses.

`make fuzz` runs each target for `FUZZ_SECONDS` over a working corpus in the
build tree (`fuzz-corpus/`), seeded from the source corpora, writing crashes
to `fuzz-artifacts/`. The source tree's corpora change only when someone
decides a finding is worth keeping. No clean target removes the working
corpus or the artifacts: an input not yet looked at is the one thing a
fuzzing run produces that cannot be made again.

## Coverage

`make coverage` configures a disposable tree inside the build tree with
`--enable-coverage --disable-shared` (so every object's counts sit beside
it rather than in libtool's hidden directory), runs `make check` there, and
writes a report per source file of `lib/` and `cmd/relic/`: lines executed,
functions entered, every function never entered by name, and every range of
lines never executed. `tests/coverage-report.sh` writes it, and can be
pointed at any tree so configured.

A source file can be compiled into more than one object: the
allocation-failure suite compiles the whole library again under names of
its own, and that copy is the only one whose failure paths run. The report
counts a line or function as reached when any object compiled from the file
reached it, so it describes what the suite as a whole reached.

Coverage is a gap finder, not a target, and nothing fails on a percentage.
The action on the report is to name each function never entered and each
large uncovered block, and either test it or record why it is untested in
`TODO.md`.

## Sanitizers

`make check-asan`, `check-lsan`, `check-msan`, `check-ubsan` and
`check-tsan` each configure a fresh disposable tree with the sanitizer's
configure option and nothing carried over but the compilers, build it and
run the whole suite under it, exporting the sanitizer's options so a report
fails the run. The options themselves are in `Makefile.am`; the one that
matters to design is that allocator-replacing sanitizers return NULL on an
allocation they will not satisfy, so relic's own failure handling is what
gets tested. A sanitizer the toolchain cannot
provide makes its target say so and succeed, unless `RELIC_STRICT_CHECK=yes`
and the target is not an allowed skip. The targets want `make -j`, since the
inner `make check` is otherwise serial.

## Running it

`make check` runs the unit, language, equivalence and structural suites,
the random self-test and a fixed set of seeds, and the replay of every fuzz
corpus. `make check-random` runs `RELIC_SEEDS` seeds from
`RELIC_SEED_START`, a thousand from one by default.
`tests/random/random -p -n 1 -s N` prints the program seed `N` generates.

The extended workflow runs both at length every week, under AddressSanitizer
and UndefinedBehaviorSanitizer: the random driver over ten thousand seeds,
each new run taking the next range so the weeks together cover new programs
(a rerun repeats its range), and every fuzz target for fifteen minutes.
`make check-random` and `make fuzz` run under the sanitizer options of the
sanitizer targets, so a long run is as strict as `make check-asan`. Both jobs are part of the [release
gate](design-release.md#the-release-gate).

## Alternatives not taken

* **Filtering generated programs through relic's own safety and
  stratification checks.** That would make the library the judge of its own
  analyses; a program valid by construction lets a refusal count as a bug.
* **A coverage percentage gate.** It rewards tests that execute lines
  without checking them; the report is read for gaps instead.
