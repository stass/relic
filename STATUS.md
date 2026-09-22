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

Queries are not yet validated. Their predicates are resolved, so a typo in a
query is reported as an undefined predicate, but a query's variables are
numbered and checked when S5 compiles it into the synthetic `$q0` rule of
architecture section 11. `TODO.md` records this. Stratification arrives in S6,
so a program with recursion through negation is still accepted.

**The relation store, stage S4.** Somewhere to put tuples, specified in
`specs/design-relations.md`. Nothing evaluates yet, so nothing in the shipping
library inserts one; the store is reached only by its unit suite until S5's
evaluator arrives.

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
that removes, reorders or renumbers a tuple -- which is what lets a semi-naive
delta be a range of identifiers instead of a relation of its own.

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
* the installed shared library exports exactly the twelve documented entry
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

There is no evaluator or query interface yet.

## Documents

| File | Contents |
| --- | --- |
| `specs/architecture.md` | target design, v0.2 |
| `specs/design-memory.md` | arenas, arrays, interning tables, the value table |
| `specs/design-parser.md` | lexical structure, the syntax tree, diagnostics and error recovery |
| `specs/design-program.md` | predicate identity, variable numbering, the compiled rule, safety |
| `specs/design-relations.md` | the tuple store, tuple identifiers, the full-tuple index, ranges, limits |
| `specs/style.md` | code style, binding on all relic-owned code |
| `relic-plan.md` | fifteen stages, six milestones, decision record |
| `initial-spec.md` | the original Draft v1, kept for reference |

## Next

Stage S5, the plan representation, the executor and the command-line tool:
end to end, where positive recursive Datalog answers queries from the command
line and `relic -q 'path(a, X)?' graph.dl` prints three symbols. It owes
`specs/design-plan.md` and `specs/design-eval.md` before its code, and it is
milestone M1.

## Decisions taken

Recorded as D-1 through D-16 in `relic-plan.md` section 1. The three that
constrain everything else:

* **D-3** tuples are dense arrays of 32-bit interned value identifiers;
* **D-8** the plan representation and the executor exist from S5 and are not
  rewritten by later optimization stages;
* **D-9** the naive evaluator ships in the library as the differential oracle.
