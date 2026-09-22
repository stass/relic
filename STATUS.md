# relic status

A snapshot of what exists. Not a roadmap: for sequencing see `relic-plan.md`,
for open work see `TODO.md`, for the target design see `specs/architecture.md`.

## Implemented

**The build skeleton, stage S0, milestone M0.** The tree configures, builds,
tests, installs and distributes on macOS; `relic` prints its version and
rejects anything else as a usage error.

| Component | State |
| --- | --- |
| `configure.ac`, `Makefile.am`, `autogen.sh` | autoconf 2.69, automake 1.13, libtool 2.4.2 or newer; one non-recursive `Makefile.am` |
| `m4/` | five local macros: C11 dialect, compiler flags, sanitizers, libedit, debug and assertion options |
| `cmd/relic/main.c` | `-V` only; unknown options and operands are usage errors |
| `compat/` | `compat.h` and replacements for `reallocarray`, `strlcat`, `strlcpy`, `strtonum`, each linked only where the platform lacks it and compiled everywhere by the test suite |
| `librelic.pc.in` | pkg-config module `librelic` |

**The foundations, stage S1.** The storage and reporting primitives every
later stage sits on, specified in `specs/design-memory.md`. No Datalog yet.

| Component | State |
| --- | --- |
| `lib/arena.c` | chunked region allocator; sticky failure flag, cleared by a reset and by the context-arena funnel |
| `lib/util.{h,c}` | `relic_grow_array()` over `reallocarray()`; one open-addressing identifier table, reserved and placed as separate steps; hashes with compile-time seeds |
| `lib/symbol.c` | byte strings interned to 32-bit identifiers; bytes in the context arena, embedded NULs preserved |
| `lib/value.c` | the value table and `relic_dom_t` of D-3; interning for all three types, the language order of section 10.1 and the total order of section 61 |
| `lib/diag.c` | diagnostic callback, last-error buffer, and a stderr formatter that escapes non-printable bytes |
| `lib/api.c` | context lifecycle and the context-arena allocation funnel |
| `include/relic.h` | `relic_t`, `relic_value_t`, `relic_location_t`, `relic_config_t`, the diagnostic callback, `relic_create()`, `relic_create_config()`, `relic_destroy()`, `relic_diag_set()`, `relic_diag_stderr()`, `relic_error()`, `relic_string()` |
| `compat/queue.h` | bundled OpenBSD copy, provenance and patches in `compat/README.md` |
| `tests/` | the unit suites for each module, plus the tree checks |

A configuration is accepted and stored but not enforced: nothing allocates
relation or index storage for a limit to apply to yet, and no `flags` value is
defined. The header says so. `compat/tree.h` is not bundled, because no
subsystem calls for a red-black tree yet.

**The front end, stage S2.** Source text becomes an abstract syntax tree and
bad source text becomes diagnostics, specified in `specs/design-parser.md`.
`relic -c` reads files or standard input, reports what is wrong with them and
evaluates nothing.

| Component | State |
| --- | --- |
| `lib/lex.c` | byte-oriented lexer over its own classification table; every token class of architecture section 13, string escapes including `\xHH`, `strtoll` integer literals with a range check, `%` comments, line and column on every token |
| `lib/parse.c` | recursive descent over the grammar; error recovery synchronizing on `.` and `?` with one diagnostic per statement; the loading entry points and the whole-stream reader |
| `lib/ast.h` | terms, atoms, literals, statements and the program list; a fact is a rule with an empty body |
| `include/relic.h` | adds `relic_load_file()`, `relic_load_path()`, `relic_load_string()` |
| `cmd/relic/main.c` | `-c` with file operands and standard input; `-V` unchanged |
| `tests/lang/` | golden language tests run by `tests/langtest.sh` as automake's `DL_LOG_COMPILER`, with `tests/langfiles.sh` failing the build if one is not registered |

Constants are interned as they are parsed, so a term holds the same domain
identifier a tuple column will hold. `not` is the negation keyword only where
a literal may begin, so `status(foo, not).` is an ordinary fact. A load is all
or nothing: a program never holds half of a file.

A bare `relic file.dl` is a usage error, because evaluating a program is what
it will mean and checking is not.

**The program model, stage S3.** The syntax tree becomes a validated program,
specified in `specs/design-program.md`. `relic -c` now checks what it loads.

| Component | State |
| --- | --- |
| `lib/pred.c` | predicates interned by `(name, arity)` through the same identifier table the symbol and value pools use; first-mention location, definedness, and the undefined-predicate sweep |
| `lib/rule.c` | rule compilation in one pass: predicate resolution, variable numbering into the slots of architecture section 25, the range-restriction check, the singleton warning, and `relic_check()` |
| `lib/program.h` | the compiled forms: arguments, atoms, comparisons, literals, variables, rules, and the program that holds them |
| `include/relic.h` | adds `relic_check()` |
| `cmd/relic/main.c` | `-c` validates after loading, and skips validation when a load failed |
| `tests/lang/` | twelve more golden tests: the accepted and rejected examples of architecture sections 10 and 14, both variable-limit boundaries, predicate identity, singletons with their exemptions, the undefined-predicate warning, and the architecture section 72 program |

Safety is one set computation, not a search: a rule is safe when the variables
of its head, its negated literals and its comparisons all occur in a positive
relational body literal. `_` is a wildcard in a positive body literal and takes
no slot; anywhere else it can never be bound and the rule is rejected. Checking
is incremental -- a cursor records the last statement compiled, so a check
compiles only what was loaded since the previous one, reports each diagnostic
once, and returns its previous answer when there is nothing new. A program that
has ever held a rejected rule stays refused.

**The relation store, stage S4.** Somewhere to put tuples, specified in
`specs/design-relations.md`.

| Component | State |
| --- | --- |
| `lib/relation.c` | the dense tuple array with the arity as its stride, the canonical full-tuple hash index, `relic_relation_insert()` returning newness, membership, reading a tuple by identifier, iteration by identifier range, and the tuple and memory limits |
| `lib/relation.h` | the tuple identifier, the half-open tuple range, the relation and the table of pointers to them |
| `lib/util.{h,c}` | adds `relic_hash_words()`, and the two pure projections the memory limit needs: the capacity a growable array would reach and the bytes an identifier table would hold after a reservation |
| `include/relic.h` | unchanged; `max_tuples` and `max_memory` are now enforced, and the header no longer claims otherwise |
| `tests/unit/test_relation.c` | the set property, growth, ranges and deltas, arity 0 and arity 32, relation ownership, both limits with their golden diagnostics, and a million tuples inside a memory envelope |

A relation is a set, and that is the property everything above it rests on: an
insertion reports newness, which is how a fixed point is detected. The store
is append-only by construction rather than by rule -- it offers no operation
that removes, reorders or renumbers one tuple, and from S8 the only removal is
a discard of every relation at once, between evaluations -- which is what lets
a semi-naive delta be a range of identifiers instead of a relation of its own.

A relation never moves. The table holds pointers to individually allocated
relations, so creating one cannot invalidate another, and a plan or an
evaluation may resolve a relation once and hold it. Storing the relations
inline was tried first and was wrong: it made every holder responsible for not
keeping a pointer across a call that might create a relation, and the first
thing that list cost was a use-after-free at the ninth predicate.

Arity zero needs no case of its own: the hash of an empty key is defined and
the comparison of two empty keys is true, so a relation of arity zero holds
zero or one tuple through the ordinary path.

Both limits are checked against what a relation would hold after an insertion
and before anything is modified, so a refused insertion leaves the relation
exactly as it was. The memory limit counts allocated capacity rather than the
smaller number a tuple count implies, since a limit that ignored the
difference would allow twice what was asked for.

**The plan, the executor and the tool, stage S5, milestone M1.** Positive
recursive Datalog answers queries from the command line, specified in
`specs/design-plan.md`, `specs/design-eval.md` and `specs/design-cli.md`.
`relic -q 'path(a, X)?' graph.dl` prints `b`, `c` and `d`.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | the plan representation of D-8 and the textual planner: positive literals in written order, each a `SCAN` or, when every argument is bound, a `MEMBER`; each comparison a `FILTER` at the earliest point its operands are bound; the head an `EMIT`; a plan is refused unless every body literal became exactly one operation |
| `lib/eval.c` | the executor, one loop over a level and a resume flag with no recursion; the naive fixed point of D-9; `relic_materialize()` and `relic_query_run()` |
| `lib/rule.c` | a query compiles into a rule over the synthetic `$qN` predicate of D-12: its named variables in first-occurrence order are the columns, and safety is the ordinary check of a head; `relic_query_prepare()`, `relic_query_count()`, `relic_query_loaded()`, `relic_query_destroy()` |
| `lib/parse.c` | parses into any arena, so a prepared query owns its syntax tree; `relic_parse_query()` holds query text to exactly one query |
| `lib/value.c` | one definition of the total order of section 61, reached by domain identifier and, as `relic_value_compare()`, by public value |
| `include/relic.h` | adds `relic_materialize()`, `relic_query_t`, `relic_row_cb`, the five query functions and `relic_value_compare()` |
| `cmd/relic/main.c` | the phases of `design-cli.md`: load every operand or standard input, check, prepare every query, then evaluate; `-a`, `-c`, `-F`, `-o`, `-q`, `-s`, `-V`, and `-X` from S7; each `-q` is named `<query N>` in diagnostics |
| `cmd/relic/output.c` | text, csv, tsv and json, byte for byte as `design-cli.md` section 4 specifies, a string literal escaping C0 and C1 controls and bytes that are not UTF-8; `-s` through a merge sort over the library's total order |
| `lib/pred.c` | no predicate may have more than 32 arguments, enforced at interning, the one path every predicate takes |
| `tests/` | `test_plan`, `test_eval` and `test_query`; an evaluation sweep in `test_exhaustion`; `tests/cli.sh` for standard input, `-o` and write failures; twenty-two golden language tests covering every binding pattern, recursion, comparisons across types, all four formats, sorting, and every query diagnostic |

The queries a program contains are now compiled and checked by
`relic_check()`, so an unsafe query makes the program invalid exactly as an
unsafe rule does. With `-q`, only the command line's queries run; the
program's own are still checked.

Evaluation materializes the whole program before answering any query --
Magic Sets arrive in S10 -- and answers a query by running its rule once into
the answer relation, then streaming that relation. No row is delivered until
the answer is complete, so a query that exceeds a limit delivers none. The
relations record how many rules they are a fixed point of, so a query after
nothing new was loaded evaluates nothing.

`-e` arrives in S9, and `-i`, `-n` and `-v` in S12. Each is an unknown option
until it does.

**The dependency graph and stratification, stage S6.** The check knows what
depends on what and rejects recursion through negation; evaluation runs in the
order that analysis settles, specified in `specs/design-stratify.md`.

| Component | State |
| --- | --- |
| `lib/depgraph.{h,c}` | the predicate graph in compressed adjacency form, one edge per relational body literal in rule and literal order, each remembering its rule, literal and sign; strongly connected components by an iterative Tarjan search, completed dependencies first |
| `lib/stratify.c` | least strata over the condensation; one error per component holding a negative edge, naming the shortest cycle through it with a note at the literal of each edge; the schedule of components with rules, grouped by stratum, cached on the program until a check compiles another rule |
| `lib/eval.c` | materialization plans every scheduled rule, then runs the schedule component by component: the naive fixed point for a recursive component, one pass for any other; each plan owns its cursors |
| `lib/rule.c` | `relic_check()` runs the analysis after compiling and before the undefined-predicate sweep |
| `lib/arena.{h,c}` | the arena declarations in a header of their own, so the program model can hold one, with moving an arena by assignment stated as part of its contract; `relic_arena_allocarray()` refuses a size that overflows |
| `tests/` | `test_stratify` for component order, strata, rule grouping, independence of textual order, rejection, and a chain and a cycle of a hundred thousand predicates analysed on a one-megabyte stack; two sweeps in `test_exhaustion`, over rejecting a program and over replacing a schedule; six golden tests, reproducing architecture sections 9 and 30 exactly and fixing which cycle is named and in what order |

A program found unstratified stays refused and is not analysed again, so a
second, independent cycle introduced by a later load is not reported until
the first is gone. The command-line tool loads every operand before it checks
and reports every cycle in one run.

**Semi-naive evaluation, stage S7, milestone M2.** Recursion no longer redoes
the work it has already done, specified in `specs/design-eval.md` section 4
and `specs/design-plan.md` section 5.3.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | `relic_plan_rule()` takes a delta literal, which it places first as a `SCAN` marked as reading the delta, even when every argument is a constant; any other literal is refused as an internal error |
| `lib/eval.c` | each component is planned with its members' relations, one plan per rule, and for a recursive component under the default strategy one delta variant per positive literal over a member; the semi-naive fixed point runs every plan once, then the variants round after round, each member's delta the identifier range it gained in the round before, until a round adds nothing; the naive fixed point stays behind `RELIC_F_NAIVE`; the executor counts iterations, derivations, insertions and scanned tuples |
| `include/relic.h` | `RELIC_F_NAIVE`; `relic_stats_t` and `relic_stats()`, the cumulative counters and a snapshot of tuples, storage and values by type |
| `cmd/relic/main.c` | `-X naive` and `-X stats`, the counters printed to standard error when the run ends, whether or not it succeeded; any other `-X` name is a usage error |
| `tests/` | `tests/equiv/strategies.sh` runs every registered language test under `-X naive` and compares sorted output, diagnostics and status with the default; `test_eval` runs every answer scenario under both strategies and pins both strategies' counters on a chain, computed by hand; `test_plan` checks variant plans and refusals; golden tests for transitive closure over a chain, a cycle, a complete graph and a disconnected one, for rules with two and three recursive literals, and for the counters of both strategies |

The two strategies take the same number of rounds on a chain; what differs is
the work. Over a six-edge chain the default derives 38 tuples and scans 216,
the naive fixed point 118 and 694, and the golden counters fail if the default
regresses toward the naive figures.

**Negation execution, stage S8.** `not` works, specified in
`specs/design-plan.md` sections 3 and 5 and `specs/design-eval.md` sections 5
and 6. The architecture section 72 program evaluates correctly under both
strategies.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | `RELIC_OP_ANTI_MEMBER`; a comparison and a negated literal are both tests, each placed at the earliest point every slot it reads is bound, in written order among tests placeable at one point; a negated literal is never a delta literal; the negation refusal is gone |
| `lib/eval.c` | the executor runs `ANTI_MEMBER` as `MEMBER` inverted; a changed program discards every relation and is evaluated again from nothing, for every program rather than only one that negates; the answer stream stops at an identifier its relation no longer has, so a callback that breaks its contract by loading cannot make it read past a relation |
| `lib/relation.c` | `relic_relation_discard_all()` empties every relation in place and returns both limit totals to zero; relations do not move |
| `include/relic.h` | `relic_materialize()` documents the discard |
| `cmd/relic/main.c` | `-l key=value`, repeatable, brought forward from S12: `tuples`, `memory`, `magic-preds` and `magic-rules`, the last two accepted and stored but not acted on until S10; values through `strtonum()`; a malformed `-l` is a usage error naming what is wrong |
| `tests/` | `test_plan` for where an `ANTI_MEMBER` goes and what its columns read, and a negated literal refused as a delta literal; `test_eval`, under both strategies, for the task graph of section 72 with negation stacked across strata, a negating query, arity zero, constants and a negation written before its binder, a later load that makes a derived tuple false, a discard that gives back what the limits count, and a contract-breaking callback; the evaluation sweep in `test_exhaustion` now includes a negated stratum; `tests/cli.sh` fails queries on `-l` limits and rejects thirteen malformed ones, a sign or a space among them; four golden tests, all compared across strategies by `tests/equiv/strategies.sh` |

The textual planner orders only positive literals. A negated literal and a
comparison go wherever their variables are first bound, so `not done(a)` and
`not frozen` are tested before any scan and reject a rule's every binding at
no cost.

Build options: `--enable-debug`, `--enable-werror`, `--enable-asan`,
`--enable-lsan`, `--enable-msan`, `--enable-ubsan`, `--enable-coverage`,
`--with-libedit`. Test suites: `make check`, `make check-asan`,
`make check-lsan`, `make check-msan`, `make check-ubsan`.

`--with-libedit` is detected but not yet consumed: nothing links libedit until
the interactive mode arrives in S12, so a build configured with it gains no
line editing. `TODO.md` records this.

### Verified

On macOS 15 with Apple clang, in-tree and out-of-tree:

* `./autogen.sh && ./configure && make && make check` succeed;
* `make distcheck` succeeds;
* the resulting tarball configures, builds, tests and installs with no
  autotools, autoconf macros or pkg-config on `PATH`;
* `--enable-werror`, `--disable-shared` and `--disable-static` all build and
  test clean, and `make install DESTDIR=...` installs and uninstalls cleanly;
* `make check-asan` and `make check-ubsan` pass; `make check-lsan` and
  `make check-msan` report that the platform does not support them and
  succeed;
* the installed shared library exports exactly the twenty documented entry
  points and nothing else; the compatibility objects linked into it are not
  reachable.

The relation store's suites were confirmed the same way, by injecting each
fault and rerunning:

* reporting a duplicate as newly inserted, which `tests/unit/test_relation.c`
  catches as a second insertion adding a tuple;
* comparing all but the last column of a tuple, which it catches as a
  five-thousand-tuple relation collapsing;
* a rehash that disagrees with the hash the insertion used, which it catches
  as every tuple becoming unfindable after a growth;
* counting the tuple limit per relation rather than across all of them, which
  it catches as the limit admitting more than it allows;
* checking the memory limit after the growth rather than before, which it
  catches as storage passing the allowance;
* an off-by-one in the end of a relation, and a range measured from the wrong
  first identifier, which it catches as a delta naming the wrong tuples;
* leaving the storage total unreconciled, and growing the column array four
  times as fast, which it catches as the total disagreeing with the relation
  and as the million-tuple envelope being left;
* storing a tuple's columns before the index has room for it, which
  `tests/unit/test_exhaustion.c` catches as a relation holding a duplicate
  after an allocation failure;
* storing relations inline in a growable table, which
  `tests/unit/test_relation.c` catches as the first relation moving, and
  losing its tuple, at the ninth predicate.

Two claims that no test reached until one was written for each are now
covered by `tests/unit/test_util.c`: that the capacity a growth reaches is
the capacity its predictor named, which is what makes the memory limit exact
rather than approximate, and that a sequence of words hashes to a stated
number, which is what would make a platform of the other byte order fail
where the difference is rather than somewhere far away.

The checker's own suites were confirmed to fail when the property they guard
is broken, rather than only to pass, by injecting each fault and rerunning:

* giving `_` a slot in a positive body literal, which
  `tests/unit/test_rule.c` catches as two wildcards taking two slots;
* never advancing the compile cursor, which it catches as a second check
  repeating the first check's diagnostics and doubling the rule count;
* numbering slots body-first rather than as written, which it catches as the
  wrong spellings in the first three slots;
* keying the predicate table on the name alone, which
  `tests/unit/test_pred.c` catches as `foo/1` and `foo/2` sharing an
  identifier;
* advancing the cursor past a statement a check ran out of memory on, which
  `tests/unit/test_exhaustion.c` catches as a statement that is never
  compiled at all.

Five of the tests are checks on the tree rather than on the engine, and each
was confirmed to fail when the property it guards is broken, rather than only
to pass:

* `tests/globals.sh` reports any C object in a writable section, and was
  checked against an injected `static` counter and an injected
  `_Thread_local` one, in both plain and instrumented builds;
* `tests/langtest.sh` was checked against an altered golden message and an
  altered expected exit status;
* `tests/langfiles.sh` was checked against an unregistered `.dl` file and
  against a file in `tests/lang/` belonging to no test;
* `tests/unit/test_exhaustion.c` compiles the library against an allocator
  that refuses the nth request and sweeps n across interning and across a
  load, asserting that the sweep reaches both a load that runs out of memory
  and one that completes, so neither branch can quietly stop being taken;
* `tests/ascii.sh` walks the whole tree and excludes what is generated.

No platform other than macOS has been exercised, because there is no
continuous integration yet. `TODO.md` records that as the highest open item,
and records the two checks whose behaviour on ELF is inferred rather than
observed.

The evaluation suites were confirmed to fail when the property they guard
is broken, by injecting each fault and rerunning:

* a fixed point that stops after one pass, which the recursion tests catch as
  missing paths;
* a scan that does not restart its range when it is entered afresh, which
  every join catches as missing answers;
* a comparison placed before its operands are bound, which
  `tests/unit/test_plan.c` and the comparison tests catch;
* an unordered pair satisfying `<=`, which `tests/unit/test_eval.c` catches as
  symbols passing an integer comparison;
* relations never extended after a later load, which it catches as a stale
  answer;
* a query's synthetic head not counted as an occurrence, which every query
  catches as a spurious singleton warning;
* a refused query consuming a serial number, which
  `tests/unit/test_query.c` catches as a misnamed answer predicate;
* rows streamed before evaluation has succeeded, which
  `tests/unit/test_exhaustion.c` catches as rows from a failed query;
* a separator written before a query is known to succeed, and a JSON string
  whose literal is not escaped, which `tests/cli.sh` and the json golden test
  catch;
* a C1 control or a byte that is not UTF-8 written raw inside a literal, which
  the text and json golden tests catch;
* a predicate wider than 32 arguments admitted by interning, which
  `tests/unit/test_pred.c` catches;
* a negated literal left out of a plan, which the planner's completeness
  check turns into an error instead of wrong answers.

The analysis suites were confirmed the same way:

* scheduling components in completion order without sorting by stratum,
  negation not lifting a stratum, and components scheduled in reverse, which
  `tests/unit/test_stratify.c` catches as schedules out of order;
* reporting every negative edge of a component rather than the first,
  choosing a component's first edge in graph order rather than text order,
  and reporting components in completion order rather than text order, which
  the golden cycle tests catch as a repeated error or a misplaced one;
* rejecting nothing because negation is ignored, which every unstratified
  test catches;
* tearing the old schedule down before the new one is complete, and
  installing a schedule an analysis could not finish, which
  `tests/unit/test_exhaustion.c` catches as a lost schedule and a check that
  passes;
* never marking a component recursive, which the recursion tests catch as a
  fixed point stopping after one pass;
* a schedule left stale after a later load, and a rejection that is not
  sticky, which `tests/unit/test_stratify.c` catches as missing rules and a
  repeated diagnostic;
* a recursive Tarjan search, which the long-chain test catches by crashing on
  its one-megabyte stack, through the chain and the cycle each alone.

The semi-naive suites were confirmed the same way:

* delta variants that never run, which `tests/equiv/strategies.sh`, the
  closure tests and `tests/unit/test_eval.c` catch as missing answers;
* a delta scan that reads the whole relation, which is correct and slower,
  and which the pinned counters in `test_eval` and `stats-semi-naive` catch as
  the extra work;
* a variant for only the first recursive literal of each rule, which the
  `joined` case of `closure-double` and the equivalence suite catch; the
  transitive closures alone did not, because a path can be split either way;
* advancing only the first member's delta, which mutual recursion catches in
  `query-recursion`, `components`, `test_eval` and the equivalence suite;
* reporting exhaustion once per refused array rather than once per
  evaluation, which the wide sweep in `test_exhaustion` catches as two
  messages;
* the equivalence suite itself run against a missing source directory and
  against a tool that always fails, both of which it reports rather than
  passing.

The negation suites were confirmed the same way:

* an `ANTI_MEMBER` that is not inverted, which every negation golden test,
  `test_eval`, the evaluation sweep and the equivalence suite catch;
* a negated literal placed before its variables are bound, and one never
  placed at all, which `test_plan` catches, the second also as the planner's
  completeness error in every negation test;
* a later load that does not discard, which `test_eval` catches as a stale
  `ready(build)` and as a limit exceeded by the tuples an earlier evaluation
  held;
* a discard that keeps the tuple total, which `test_eval` catches as a limit
  exceeded after the discard;
* the answer stream without its guard, which the contract-breaking callback in
  `test_eval` catches by crashing.

## Documents

| File | Contents |
| --- | --- |
| `specs/architecture.md` | target design, v0.2 |
| `specs/design-memory.md` | arenas, arrays, interning tables, the value table |
| `specs/design-parser.md` | lexical structure, the syntax tree, diagnostics and error recovery |
| `specs/design-program.md` | predicate identity, variable numbering, the compiled rule, safety |
| `specs/design-relations.md` | the tuple store, tuple identifiers, the full-tuple index, ranges, limits |
| `specs/design-plan.md` | the operations, the binding decisions a plan records, the textual planner, a query as a rule |
| `specs/design-stratify.md` | the dependency graph, components, strata, the unstratified diagnostic, the schedule |
| `specs/design-eval.md` | the executor, the naive and semi-naive fixed points, materialization, answering a query |
| `specs/design-cli.md` | the phases of an invocation, which queries run, exit statuses, the output formats |
| `specs/style.md` | code style, binding on all relic-owned code |
| `relic-plan.md` | fifteen stages, six milestones, decision record |
| `initial-spec.md` | the original Draft v1, kept for reference |

## Next

Stage S9, indexes, statistics, planner and explain, milestone M3:
column-bitmask hash indexes, access path selection, greedy body ordering,
relation statistics and `-e`, each with `-X` switches and equivalence tests
against the textual planner. `specs/design-index.md` and
`specs/design-planner.md` come first.

## Decisions taken

Recorded as D-1 through D-16 in `relic-plan.md` section 1. The three that
constrain everything else:

* **D-3** tuples are dense arrays of 32-bit interned value identifiers;
* **D-8** the plan representation and the executor exist from S5 and are not
  rewritten by later optimization stages;
* **D-9** the naive evaluator ships in the library as the differential oracle.
