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
| `tests/lang/` | twenty golden language tests run by `tests/langtest.sh` as automake's `DL_LOG_COMPILER`, with `tests/langfiles.sh` failing the build if one is not registered |

Constants are interned as they are parsed, so a term holds the same domain
identifier a tuple column will hold. `not` is the negation keyword only where
a literal may begin, so `status(foo, not).` is an ordinary fact. A load is all
or nothing: a program never holds half of a file.

Nothing yet turns the tree into a program. `relic -c` reports lexical and
syntactic mistakes only; predicate identity, variable numbering, safety and
stratification arrive in S3 and S6, so a rule with an unbound head variable is
still accepted. A bare `relic file.dl` is a usage error, because evaluating a
program is what it will mean and checking is not.

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
* the installed shared library exports exactly the eleven documented entry
  points and nothing else; the compatibility objects linked into it are not
  reachable.

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

There is no program model, relation store, evaluator or query interface yet.

## Documents

| File | Contents |
| --- | --- |
| `specs/architecture.md` | target design, v0.2 |
| `specs/design-memory.md` | arenas, arrays, interning tables, the value table |
| `specs/design-parser.md` | lexical structure, the syntax tree, diagnostics and error recovery |
| `specs/style.md` | code style, binding on all relic-owned code |
| `relic-plan.md` | fifteen stages, six milestones, decision record |
| `initial-spec.md` | the original Draft v1, kept for reference |

## Next

Stage S3, the program model and safety: the syntax tree becomes a validated
program, with the `(name, arity)` predicate table, variable numbering, the
range-restriction check and the singleton-variable warning, and `relic -c`
becomes useful. It owes `specs/design-program.md` before its code.

## Decisions taken

Recorded as D-1 through D-16 in `relic-plan.md` section 1. The three that
constrain everything else:

* **D-3** tuples are dense arrays of 32-bit interned value identifiers;
* **D-8** the plan representation and the executor exist from S5 and are not
  rewritten by later optimization stages;
* **D-9** the naive evaluator ships in the library as the differential oracle.
