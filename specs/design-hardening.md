# relic hardening design: generated programs, fuzzing, coverage, benchmarks

**Status.** Design document for the test machinery that looks for bugs the
curated corpus cannot find: a generator of random programs with a
differential driver and a shrinker, fuzz targets over the loader and the
evaluator, a coverage report, and a benchmark harness. Forward-looking, like
every document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 65.3, 65.4, 66 and 67;
`specs/design-api.md`, whose public interface everything here drives;
`specs/decisions.md` D-4, D-9 and D-19.

---

## 1. What this document settles

* how a random program is generated so that it is valid by construction --
  safe, stratified and within every structural limit -- rather than filtered
  after the fact;
* what a generated program is run under and what counts as a disagreement;
* how a failing program is reduced, and how a failure is kept from turning
  into a different failure while it is reduced;
* how a crash, a hang or a leak in the engine is reported rather than taking
  the harness down with it;
* what the fuzz targets feed the library, how their corpus is replayed by
  every `make check` on every compiler, and how they are built with libFuzzer;
* what `make coverage` measures and what is done with its report;
* what `bench/` measures and against what.

It does not change the language, the library or the tool. Everything here is
a client of the public interface of `<relic.h>`.

---

## 2. The generator

### 2.1 Programs, not strings

The generator builds a program model -- predicates, facts, rules and queries
as small arrays of terms -- and prints it. It never produces text a parser has
to accept by luck. A syntax fuzzer spends almost all its time being rejected
by the lexer; the generator spends all of its time in the evaluator, which is
where the four equivalences of architecture section 66.2 live. The front end
has its own fuzz target (section 5).

The model is the generator's own, not `lib/ast.h`. A generator that shared
the library's representation would share its mistakes: a term the parser
builds wrongly would be built the same wrong way by the generator and the two
would agree.

### 2.2 Determinism

A program is a pure function of one seed, an integer from zero to
2^63 - 1. The generator draws from a
splitmix64 stream of its own, never `rand(3)` or `random(3)`, whose sequences
differ between C libraries, so a seed reported on one platform reproduces the
same program on every other. Every size and every choice -- how many
predicates, which arities, how deep the strata, how many facts -- is drawn
from the stream, so the corpus varies its shape as well as its contents.

D-19 draws each context's hash seed at random. Nothing the harness compares
depends on it: answers are compared as sorted sets of rendered values and
relations by their tuple count, neither of which depends on where a key sits
in a table.

### 2.3 Stratified by construction

Every predicate is given a level when it is created. Facts-only predicates
sit at level zero; every derived predicate is given a level from one to
three. A rule for a predicate at level `h` may use:

* a positive literal of any predicate at a level `<= h`;
* a negated literal of any predicate at a level `< h`;
* an aggregate literal whose body reads predicates at levels `< h` only.

Along a dependency edge, from a head to its body, the level never rises, and
along a negative or an aggregate edge it strictly falls. A cycle returns to
where it started, so it cannot contain a negative or an aggregate edge.
Every generated program is stratified by this argument alone; there is no
stratification check in the generator and none is needed. Recursion and
mutual recursion arise from positive literals at the head's own level, and
deep strata from the negated ones.

A derived predicate's first rule, its base rule, reads only levels strictly
below its own. Without it, a predicate whose every rule reads itself holds
nothing, and a corpus of mostly empty relations agrees on every strategy
without testing any of them. For the same reason the proportions are tuned
so that most bodies are satisfiable: bodies share few variables, positive
literals seldom hold constants, and a body seldom has more than one
negation or comparison, and never more than two of each. Measured over three hundred seeds, about two thirds of derived
predicates hold tuples.

### 2.4 Safe by construction

A rule body is generated in two steps. First the positive relational
literals, whose arguments are drawn from the rule's variables, from constants
and from `_`. The set `P` of variables they mention is then fixed, and every
later choice draws variables from `P` only: the head's arguments, the
arguments of each negated literal and both operands of each comparison. That
is architecture section 14's safety condition, stated as the order in which
the generator makes its choices, so an unsafe rule cannot be produced. A
comparison never holds `_`, which section 14 rejects. An argument of a
negated literal is `_` as often as a positive literal's is, a column the
literal does not read, which needs no binding, `specs/design-negation.md`
section 3.

An aggregate literal is generated by the same discipline,
`specs/design-aggregates.md` section 16: its body's variables are drawn
fresh, a subset of `P` is shared with it and each shared variable is placed
in a positive literal of the body, so the group rule is safe and every
shared slot is bound; its result is added to `P` and read by the head or a
later comparison. A `sum` reads only predicates whose integers are small
enough that no group's total can overflow, so no generated program fails
on the data.

Calls of the string functions are generated by the same discipline, and are
defined by construction as well as safe, `specs/design-strings.md` section
15: a call's string arguments are variables of `P` or constants from a
small pool of short strings, its integer arguments small constants or
variables of `P`, its patterns drawn from a fixed list of valid ones, and the strings a program states are short enough that
no `cat` of the depth the generator nests can reach the length limit. A
call stands in a comparison, as a filter over `P` or as the expression an
assignment binds a fresh variable to, which joins `P`. So no generated
program meets an evaluation error, every strategy is compared on every
program, and a disagreement is a bug.

Arity is at most four and a rule mentions at most six variables, far inside
D-7's limits, so no structural limit can refuse a generated program.

### 2.5 What is varied

* **Arity** from zero to four, including zero-arity predicates.
* **Recursion**: self-recursion, mutual recursion among predicates at one
  level, and rules with more than one recursive literal.
* **Strata**: negation across up to four levels, over negated literals
  holding `_` as well as ones naming every column.
* **Values**: a small pool of symbols, integers (including zero, a negative
  value and both extremes of `int64_t`) and strings (including the empty
  string and bytes that need escaping), so that joins find matches and
  columns mix types.
* **Comparisons**: all six operators between variables and constants of every
  type, so cross-type ordering (D-11) is exercised.
* **Calls**: each of the six string functions, nested, as a filter's
  operand and as an assignment's value, over operands of the wrong type as
  well as the right one, so that a call without a value filters its
  binding under every strategy alike.
* **Predicates with facts and rules both**, which the Magic rewrite reads on
  demand rather than completely.
* **Queries**: a single atom under a random binding pattern, including
  repeated variables and fully bound atoms, or a conjunctive body with
  negation and comparisons.

### 2.6 Invalid output is a failure

The driver installs a diagnostic callback. A generated program that the
library refuses with an error is reported as a failure of its own kind: the
generator's construction and the library's safety and stratification analyses
disagree, and one of them is wrong. Warnings -- a singleton variable, a
predicate never defined -- are expected and ignored.

---

## 3. The differential driver

### 3.1 Strategies

Each program is loaded and checked once per strategy, in a context of its own
created with `relic_create_config()`, and each of its queries is collected
with `relic_query_collect()`. The strategies are those of
`tests/equiv/strategies.sh`, plus three:

| Strategy | Flags | Tuple total held to |
| --- | --- | --- |
| default | none | -- |
| naive | `RELIC_F_NAIVE` | default |
| no-index | `RELIC_F_NO_INDEX` | default |
| no-stream | `RELIC_F_NO_STREAM` | default |
| no-exists | `RELIC_F_NO_EXISTS` | default |
| parallel | `RELIC_F_PARALLEL_ALWAYS` | default |
| no-reorder | `RELIC_F_NO_REORDER` | -- |
| no-magic | `RELIC_F_NO_MAGIC` | -- |
| group-rules | `RELIC_F_GROUP_RULES` | -- |
| naive+no-magic | `RELIC_F_NAIVE \| RELIC_F_NO_MAGIC` | no-magic |
| no-index+no-magic | `RELIC_F_NO_INDEX \| RELIC_F_NO_MAGIC` | no-magic |
| no-reorder+no-magic | `RELIC_F_NO_REORDER \| RELIC_F_NO_MAGIC` | no-magic |
| no-exists+no-magic | `RELIC_F_NO_EXISTS \| RELIC_F_NO_MAGIC` | no-magic |
| parallel+no-magic | `RELIC_F_PARALLEL_ALWAYS \| RELIC_F_NO_MAGIC` | no-magic |
| group-rules+no-magic | `RELIC_F_GROUP_RULES \| RELIC_F_NO_MAGIC` | -- |
| materialized | `RELIC_F_GROUP_RULES`, and `relic_materialize()` before the queries | group-rules+no-magic |

The `group-rules` strategies compile every aggregate body to a group rule
where the default folds a body that is one atom of distinct variables in
place, `specs/design-aggregates.md` section 5.6, so a fold's column map is
checked against the copy it replaces on every generated program. They hold
the copies the other strategies do not, so their totals are held to no
other strategy's but each other's. The derivation oracle of
`tests/random/compare.c`, which counts one set of derivations per group
body, counts the materialized strategy's derivations, which runs under
this flag, where every body derives, rather than knowing which bodies the
compiler folds in place.

Every strategy's answers must equal the default's. That covers all six
equivalences: `semi-naive == naive`, `magic == full materialization`,
`indexed == no-index`, `reordered == textual order`, `existence ==
no-exists` and `deferred == serial`, the last the rounds of
`specs/design-parallel.md` whose insertions wait for their end. The
materialized strategy adds another
path through the evaluator, a query run over
relations that already hold the fixed point, which `specs/design-magic.md`
section 9 specifies and nothing else in the differential harness reaches.
The no-stream strategy gathers every answer in its relation, as a query
whose plan may derive a row twice must, so a query whose plan derives each
row once is answered both ways; that the rows also come in the same order
is the claim of `tests/equiv/streaming.sh`, since this harness compares
answers as sets.

Answers alone do not see a relation a query does not read. Tuple totals do:
after each query, `relic_stats()` gives the tuples every relation together
holds, and a strategy that evaluates the same rules as its reference must
hold the same number. The table's third column states which pairs evaluate
the same rules. The reasoning is the one `tests/equiv/strategies.sh` gives:
the naive fixed point is complete, so its total is the size of the least
model of its rules, and the others insert only tuples those rules justify, so
equal totals mean equal relations. `no-reorder` is held to answers only,
because the planner's order is the sideways information passing strategy of
the rewrite, so with the rewrite it evaluates different generated rules.

Neither answers nor totals see a derivation made twice, since a duplicate
changes neither. The materialized strategy's derivation count does. Its
expected value is one derivation per fact that seeds a derived relation,
plus one per distinct binding of each rule body's variables over the fixed
point, and of each group body's, which a context of its own counts as the
rows of one query per body under the naive fixed point without the
rewrite. A program with `_` in a positive literal is not counted, since
one binding may then match several tuples; nor is one whose positive
literal holds a variable its rule mentions nowhere else, since such a
literal derives once per tuple or once per binding as the order makes it
a generator or an existence check, `specs/design-existence.md`. `_` under
`not` multiplies nothing, since a negated literal lets a binding through
at most once.

A program whose negated literals hold `_` is also loaded in its helper
rendering, `specs/design-negation.md` section 9, and run under the default
strategy: each such literal negates a helper relation of its named columns
instead, derived by a positive literal holding the `_`. Its answers must
equal the default's, which holds the probe of a negated literal to the
meaning of `_` under `not` by a path that never takes the probe, so a
mistake every strategy shares is still found. Tuple totals are not
compared, since the helper relations hold tuples the program has nowhere.

### 3.2 Answers compared as text

A value's identifier belongs to the context that interned it, so rows from
two contexts cannot be compared as `relic_value_t`. Each row is rendered to
text -- a symbol as its spelling, a string quoted with every byte outside
printable ASCII escaped, an integer in decimal -- and the rendered rows of an
answer are sorted bytewise. Two answers are equal when their sorted renderings
are, which is set equality, the semantics architecture section 11 gives a
query.

### 3.3 Isolation

The engine under test may crash, hang, leak or trip a sanitizer, and each of
those is a finding rather than a reason for the harness to stop. Each program
is therefore evaluated in a child process created with `fork(2)`:

* the child runs every strategy, prints what disagreed on its standard error,
  and tells the parent over a pipe which strategy it is about to run and,
  once it has one, its verdict;
* an alarm in the child turns a hang into a failure of its own kind;
* a child killed by a signal is a crash, charged to the strategy it last
  announced;
* a child that has sent its verdict exits through `exit(3)`, so a
  LeakSanitizer build checks it for leaks; any nonzero status, or an exit
  with no verdict, is abnormal, whatever the verdict said.

The attribution holds by construction: the parent learns which strategy
failed from the order of the child's own announcements, not from anything
the dying child has to manage to say. The exit status carries nothing but
success or failure, so no status a sanitizer or the C runtime chooses can
be mistaken for a verdict. That is also what the shrinker needs: whether a
candidate still fails, and how.

### 3.4 Failure kinds

A failure is a kind and, where one applies, the strategy that disagreed:

* **rejected** -- the library refused a generated program;
* **answers** -- a strategy's answer differs from the default's;
* **totals** -- a strategy's tuple total differs from its reference's;
* **query** -- a query failed to collect under some strategy, with no limit
  configured;
* **derivations** -- materializing made a derivation twice, or missed one,
  section 3.1;
* **helper** -- the helper rendering of section 3.1 answered a query
  otherwise than the program, charged to no strategy, since every strategy
  agreed;
* **crash** -- the child was killed by a signal;
* **hang** -- the child ran past its alarm;
* **abnormal** -- the child exited with a nonzero status or without a
  verdict, which is how a sanitizer that reports and exits, rather than
  aborting, shows up;
* **evaluation** -- an evaluation failed on a result the language leaves
  undefined: an integer overflow, a zero divisor, a string longer than a
  string value can hold, or a pattern that is not a regular expression.

The last kind covers the evaluation errors of `specs/design-arithmetic.md`
section 4.2 and `specs/design-strings.md` section 4.3, told apart from a
query that failed for another reason by the error's class,
`specs/design-errors.md` section 3, and not by its text. A generated
program is defined by construction, section 2.4, so an evaluation error is
a failure of the generator's construction or of the library's evaluation;
the report quotes the error, which says which of the four it met.

A crash also records its signal and an abnormal exit its status, so that a
reduction keeps, say, a failed assertion from sliding into a plain
segmentation fault. The distinction is only as fine as the signals: under a
sanitizer built to abort, every report ends in the same signal, and one that
exits instead ends every report with the same status.

---

## 4. The shrinker

### 4.1 Why it matters more than the generator

A generated counterexample is a curiosity; its five-rule reduction is a bug
report. The shrinker reduces a failing program greedily, one deletion at a
time, and keeps a deletion only when the candidate still fails with the same
kind, the same strategy and, for a crash or an abnormal exit, the same signal
or status. Holding the failure constant is what stops a
reduction from sliding from one bug to an easier, unrelated one -- or to a
candidate that fails only because a deletion made it invalid. A refused
program is the exception: the first strategy to load it always refuses it,
so its reduction holds only the kind, and may move from one reason for
refusing to another. An evaluation error is held the same way, by its kind
and strategy, and may move from one of the four errors to another.

### 4.2 Edits

Every edit removes something, in this order, from the largest to the
smallest:

1. a query, while at least one remains;
2. a rule or a fact;
3. a literal from a rule body or a query body;
4. an argument position from a predicate everywhere it is used, which
   shortens every atom of that predicate by one column.

A pass tries every edit of every kind; the shrinker repeats passes until one
changes nothing. An edit can make a candidate invalid -- removing the
positive literal that bound a variable makes the rule unsafe -- so each
candidate is checked for safety in the model before it is run, and an unsafe
one is skipped. Deletions add no dependency edge, so stratification is
preserved without a check. Removing an argument position keeps every level
and every sign, so it too keeps stratification.

The shrinker runs each candidate exactly as the driver runs a program, in a
child, so a crash or a hang shrinks as readily as a wrong answer.

### 4.3 The harness is tested too

A differential harness that cannot fail passes forever. The driver has a
fault-injection mode that drops one row from the answers of one comparison
to any query with at least one row, for each comparison of answers in
turn: one strategy's, which must be reported as an **answers** failure,
and the helper rendering's of section 3.1, which must be reported as a
**helper** failure. `make check` runs it and requires that each be
reported as its kind and shrunk to a program of at most four statements;
the smallest program with a row to drop is a fact and a query. It is the
one test that shows each comparison, the isolation and the shrinker are
all connected.

The self-test shows that a disagreement is reported, not that the engine's
real mistakes produce one. That was checked when the suite was written by
planting bugs in a copy of the engine and running the suite over it: a delta
scan skipping its first tuple, a component member whose gains went
unrecorded, an index lookup stopping after one tuple, and a supplementary
Magic predicate losing a variable were each found within the first few dozen
seeds and shrunk to between two and five statements.

---

## 5. Fuzz targets

### 5.1 Four targets

All are libFuzzer entry points, `LLVMFuzzerTestOneInput()`, and the first
three are over the public interface.

* **load** takes the input as program text, loads it with
  `relic_load_string()` and checks it with `relic_check()`. It covers the
  lexer, the parser, the program model, safety, the dependency analysis and
  stratification, which are the components most exposed to input nobody
  chose.
* **eval** goes on from there. When the program checks, it answers every query
  the program holds twice, once by default and once under
  `RELIC_F_NAIVE | RELIC_F_NO_MAGIC`, and aborts if both succeed with
  different answers. This is architecture section 67's second target,
  comparing optimized and naive execution, with the fuzzer as the generator.
* **import** takes the input's first byte as a choice of format, header flag
  and column list from a small fixed table, and imports the rest as a table
  with `relic_import_file()`, through a stream whose read function is the
  target's own where the platform can make one (`specs/design-import.md`
  section 11). It covers the table reader and the importer, and aborts when
  an import fails without reporting an error.
* **regex** feeds `lib/regex.c` arbitrary patterns and strings through the
  internal interface: the input's first byte is the pattern's length, the
  pattern follows and the string is the rest, NUL and every metacharacter
  among them. A pattern must compile into no more instructions than
  `3 * length + 1` or be refused with a failure at one of its bytes, and a
  compiled pattern must match the string or not, reporting a start inside
  it, without a crash, a hang or a read outside its inputs;
  `specs/design-strings.md` section 8.4.

A diagnostic callback is installed in each, so a fuzzer's own output is not
flooded: the load target discards every message, the import target counts
the errors, and the eval target counts
the errors each call reports and notes whether the last was a limit's
refusal. A query that fails is skipped only when that call itself reported a
refusal; one that fails reporting nothing is a finding like any other. The eval target's optimized context also
installs an explain callback that discards every line, so the plan renderer
runs over every program the fuzzer builds.

Inputs are bounded twice: libFuzzer's `-max_len` bounds the text, and the
eval target sets `max_tuples` and `max_memory` so that a program deriving a
large relation is refused by a limit rather than timing out. A program with a
small relation and a large join -- a cross product whose tuples are all
duplicates -- stays inside every limit and can still run long, because no
limit counts steps; `-timeout` is the bound there, and a timeout on such an
input is not a bug in the engine. A query that fails for any reason but a
limit or an evaluation error, which the error's class tells apart, is a
finding, and aborts. An evaluation error -- an integer overflow, a zero
divisor, a string too long, a pattern that is not one -- is excused as a
limit is: the naive strategy evaluates bindings the optimized one never
reaches, so one may meet an error the other does not, which
`specs/design-arithmetic.md` section 4.2 allows.

### 5.2 Replayed by every build

A fuzz target is compiled twice. With `--enable-fuzzers`, it is linked with
`-fsanitize=fuzzer` into a libFuzzer program. Always, it is also linked with
a small replay driver whose `main()` passes each file named on its command
line, or each file in a named directory, to the entry point once. `make check`
runs every target's replay program over its corpora, so every input that
ever mattered is replayed on every compiler and under every sanitizer target,
including platforms with no libFuzzer at all.

### 5.3 The corpus

The corpus of the load and eval targets is `tests/lang/*.dl`, which already
holds every construct and every malformed input the golden suite specifies,
plus `tests/fuzz/corpus/`, which starts with inputs the golden suite does not
hold -- raw bytes the lexer must refuse, a program the Magic rewrite cuts into
supplementary predicates, and a few generated programs -- and is where an
input a fuzzer found is kept once the bug it found is fixed. A reproducer is
added there with a name saying what it found, never deleted.

The import target's input is a table rather than program text, so it has a
corpus of its own, `tests/fuzz/corpus-import/`, replayed through that target
alone: tables in both formats, with and without a header, under each column
list, well formed and holding each kind of mistake. Its findings are kept
there by the same rule. The regex target's inputs are a pattern and a
string, so it has a corpus of its own too, `tests/fuzz/corpus-regex/`:
the constructs of the dialect, the ways a pattern fails to compile, and
patterns exponential for a backtracking matcher.

The fuzz dictionary, `tests/fuzz/relic.dict`, holds the tokens a mutation
would otherwise have to find byte by byte: the punctuation, the keywords,
the aggregate and function names, and the pattern metacharacters.

### 5.4 `--enable-fuzzers`

`configure --enable-fuzzers` checks that the compiler links a program with
`-fsanitize=fuzzer` and fails if it does not; the flag is useless with a
compiler that cannot build the targets, so saying so at configure time is
better than a build that quietly omits them. It then compiles the whole tree
with `-fsanitize=fuzzer-no-link`, the coverage instrumentation libFuzzer
steers by, and only the targets with libFuzzer's `main()`. Apple's
clang has no libFuzzer; LLVM's own clang does. `make fuzz` then runs each
libFuzzer target for `FUZZ_SECONDS`, sixty by default, over a working copy of
its corpus in the build tree, so the source tree's corpus changes only when
someone decides a finding is worth keeping. The fuzzing build is combined
with `--enable-asan` and `--enable-ubsan` as the developer chooses.

---

## 6. Coverage

`--enable-coverage` compiles with `--coverage`, the gcov instrumentation
both gcc and clang provide. `make coverage` configures a tree beside the
current one without shared libraries, as the sanitizer targets do, runs
`make check` in it and writes a report per source file of `lib/` and
`cmd/relic/`: the lines executed, the functions entered, every function
never entered by name, and every range of lines never executed. The report
is also saved as `coverage.txt` in that tree; `tests/coverage-report.sh`
writes it and can be pointed at any tree built with `--enable-coverage
--disable-shared`, since a shared build keeps the library's objects apart.

A file can be compiled into more than one object. The allocation-failure
suite compiles the whole library again under names of its own, and that
copy is the only one whose failure paths run. The report counts a line as
executed, and a function as entered, when any object compiled from the file
executed or entered it, so it describes what the suite as a whole reached.

Coverage is a gap finder, not a target. Nothing fails on a percentage. The
action on the report is to name each function never entered and each large
uncovered block, and either test it or record why it is untested in
`TODO.md`.

---

## 7. Benchmarks

`bench/run.sh` generates a fixed set of workloads with `awk` -- a long chain,
a random graph, a grid, a same-generation tree, a negation-heavy task graph
and a large fact file -- runs each through a `relic` with `-X stats`, and
prints the wall-clock time with the tuples derived, held, scanned and looked
up. The chain is queried through a right-recursive and a left-recursive
closure alike, since with the first argument bound the first makes the
rewrite demand every node the query reaches and the second does not, and
the pair shows what the order of a recursive body costs.
`bench/baseline.txt` records one machine's run, named with its processor and
compiler, for comparison by eye. Timing is
machine-dependent and noisy, so nothing fails on it: `tests/perf.sh` is the
test that bounds performance, and `bench/` is the instrument used to look
closer when it fails or before a change meant to be faster.
The data sets are generated by `bench/datasets.sh`, which the comparison
with other engines, specified in `specs/design-bench.md`, shares.

---

## 8. Running it

```text
make check                   the fault-injection self-test, fifty fixed
                             seeds, and the replay of both fuzz corpora
make check-random            RELIC_SEEDS seeds from RELIC_SEED_START,
                             1000 from 1 by default; both are make
                             variables, set on the command line
tests/random/random -p -n 1 -s N
                             print the program seed N generates
make fuzz                    each libFuzzer target for FUZZ_SECONDS
make coverage                the per-file coverage report
bench/run.sh                 the benchmark workloads
```

A failure prints its seed, its kind, the strategy that disagreed, what
differed, and the shrunk program as text that `relic` runs as it stands. With
`-o DIR` the driver also writes the original and the shrunk program there.

---

## 9. What is deliberately absent

* **No limits in the differential runs.** Different strategies derive
  different amounts, so a limit would fire in one and not another and the
  comparison would be of two refusals. Generated programs are small enough
  that no limit is needed; limits are exercised by the golden suite and by
  the eval fuzz target.
* **No comparison of diagnostics.** Every strategy loads and checks the same
  text, so the warnings are identical by construction; the one that differs,
  the abandoned rewrite's warning, differs by design.
* **No syntax mutation in the generator.** Malformed input is the load fuzz
  target's job.
* **No oracle for what every strategy shares.** A differential test finds a
  bug in one strategy by comparing it with another, so a bug in something
  all of them use -- the value order, interning, the parser, the output of a
  comparison -- makes every strategy agree on the same wrong answer. The
  curated golden suite, whose expectations were written by hand, is what
  covers that code.
