# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Implement relic 1.0.** [Critical] Fifteen stages S0 through S14, specified in
`relic-plan.md`. Current position: S1 landed; S2, the lexer and parser, next.

## Documentation debt

**Design documents owed.** [High] Each stage owes one before its code:
`design-parser.md` (S2), `design-program.md` (S3), `design-relations.md` (S4),
`design-plan.md` and `design-eval.md` (S5), `design-stratify.md` (S6),
`design-index.md` and `design-planner.md` (S9), `design-magic.md` (S10). Each
is referenced from `specs/architecture.md` when written.

**`specs/style.md` readability references.** [Low] The readability standard
lists qualities to aim for but names no relic functions yet, because none
exist. Populate the reference table once S5 lands, choosing functions that
actually read well rather than whichever happen to be there.

## Build and packaging

**Continuous integration is not configured.** [High] `relic-plan.md` section 6
specifies thirteen jobs; the repository has none, so every claim about a
platform other than macOS is untested. The tarball job matters most, because
it builds what users actually download, and the Linux jobs matter next,
because leak detection does not work under AddressSanitizer on macOS and
`make check-lsan` has therefore never run. The BSD and musl jobs need
third-party VM actions and can follow.

**Manual pages are not written.** [Medium, component: `man/`] `relic.1` and
`relic.3` are S12 and S14 deliverables, so `dist_man1_MANS` and
`dist_man3_MANS` are absent from `Makefile.am` and the installed set of
`specs/architecture.md` section 65.5 is short two files. The `mandoc -Tlint`
job of `relic-plan.md` section 6 has nothing to lint until then.

**`--enable-fuzzers` is not implemented.** [Low, component: `configure.ac`]
`specs/architecture.md` section 65.3 lists it, but the fuzz targets it would
build arrive in S13. `make coverage` and `make check-random` of section 65.4
are deferred to S13 for the same reason.

**`--enable-lsan` is not in the architecture's option list.** [Low, component:
`specs/architecture.md`] Section 65.4 specifies a `make check-lsan` target but
section 65.3 lists no option to build with LeakSanitizer, so S0 added one to
make the target implementable. Decide whether section 65.3 should record it.

**The language test harness does not exist.** [Low, component: `tests/`]
`relic-plan.md` section 5 specifies `tests/langtest.sh` as automake's
`DL_LOG_COMPILER`, and S0's `Makefile.am` outline registers it. There are no
`.dl` tests to run until S2 produces a parser, and a harness with no tests
rots unnoticed, so both the harness and `TEST_EXTENSIONS = .dl` arrive with
the first language test.

**Two tree checks behave differently on ELF and have never run there.**
[High, component: `tests/globals.sh`] The no-mutable-global-state check has
two branches, and only the Mach-O one has been exercised, because there is no
Linux job yet. Two specific doubts to settle the first time it runs on ELF:
GNU `nm` reports `.data.rel.ro` as type `d`, so a constant table holding
pointers may be reported as mutable state and fail the build spuriously; and
the claim that a thread-local object is reported as ordinary data or bss is
inferred from how ELF places `.tdata` and `.tbss`, not observed. Settle both
by injecting each case on Linux, as was done on macOS, rather than by reading
the output once and assuming.

**The tarball carries no project documents.** [Low, component: `Makefile.am`]
`EXTRA_DIST` ships `README.md` and `LICENSE` but not `specs/`, `relic-plan.md`,
`STATUS.md` or `TODO.md`. For a tool whose selling point is that it can be
audited, shipping the architecture document with the source is worth
considering. Decide at S14 with the rest of packaging.

## Engine

**Semi-naive duplicate elimination.** [Low, component: `lib/plan.c`] The delta
variant scheme of D-10 derives some tuples more than once per iteration for
rules with three or more recursive body literals. The refinement that uses the
old relation for earlier literals and the new one for later literals removes
that, at the cost of being non-obvious on first reading. Measure before
implementing; rules that would benefit are rare.

**Append-only invariant versus incremental maintenance.** [Medium, component:
`lib/relation.c`] D-10 represents a delta as a range of tuple identifiers,
which requires that nothing compacts, deletes or renumbers a relation during a
fixed point. Any future incremental insertion or DRed-style deletion support
must confront this first. The invariant is asserted in `lib/relation.c`.

**Size budget.** [Medium] `relic-plan.md` totals about 7.5 KLOC against the 4
to 7 KLOC of `specs/architecture.md` section 2.2, which treats size as a design
constraint. Track actual lines per stage against the budget; the levers are
listed under risk R7.

**An unknown value type cannot be rejected in a build that checks for it.**
[Low, component: `lib/value.c`] `value_well_formed()` rejects a
`relic_value_t` whose `type` is not one of the three enumerators, which an
embedding program can construct. The guard is unreachable under
`-fsanitize=undefined`: reading an out-of-range enum is undefined, so
UndefinedBehaviorSanitizer traps on the load before the guard runs, and in
that build the library aborts its host rather than returning `-1`. The guard
is kept because it costs three lines and catches the mistake on an ordinary
release build, but it cannot be a guarantee and `tests/unit/test_value.c`
leaves the case untested for the same reason. Making it real would mean
taking the type as an integer across the public boundary, which changes the
representation `specs/architecture.md` section 19.1 specifies. Revisit if an
embedder hits it.

**A configuration is accepted but not enforced.** [Low, component:
`include/relic.h`] `relic_config_t` and `relic_create_config()` are public and
their four limits are stored and never read, because no relation or index
storage exists to apply them to until S4, and no `RELIC_F_` behaviour switch
is defined until S5. The header says so plainly so that a caller is not
misled. Confirm the field shape is right when S4 enforces the first limit;
public API is the hardest thing to withdraw.

**Assertions are absent from release builds.** [Medium, component:
`configure.ac`] `specs/architecture.md` section 65.3 makes assertions a
`--enable-debug` feature, so an ordinary build defines `NDEBUG`. Sanitizer
builds keep them, which is where they earn their place. The consequence is a
rule for later stages rather than a defect: an invariant that must hold in a
release build needs an explicit check and a diagnostic, not an `assert()`.
The append-only invariant of D-10 is the first one this applies to.

**Allocation-failure coverage stops at interning.** [Medium, component:
`tests/unit/test_exhaustion.c`] The suite compiles the library against an
allocator that refuses the nth request and sweeps n across symbol and value
interning, which is what caught the index-growth failure that broke interning
identity. Every later stage adds allocation paths that the sweep will not
reach unless it is extended: the tuple store, the indexes, the plan arena. Add
a scenario per stage rather than leaving the suite describing S1 forever.

**Leak detection is unavailable on macOS.** [Medium, component: `Makefile.am`]
AddressSanitizer on Apple silicon reports that `detect_leaks` is unsupported
and aborts, so `configure` probes for it and `make check-asan` asks for leak
detection only where it works. `make check-lsan` is unavailable there for the
same reason. Until the Linux continuous integration jobs exist, no leak check
runs anywhere, which matters from S1 onward because that is where the tree
starts allocating.

**`--with-libedit` is detected but not consumed.** [Low, component:
`Makefile.am`] `configure` finds libedit and reports it, and nothing links
it, because the interactive mode that would use it arrives in S12. A build
configured with `--with-libedit=yes` therefore succeeds and offers no line
editing. `STATUS.md` says so; the option becomes true when S12 lands.

**`librelic.pc` declares no private dependencies.** [Low, component:
`librelic.pc.in`] There are none today. The moment the library links anything
of its own, libedit included, `Libs.private` and `Requires.private` have to
name it or `pkg-config --static --libs librelic` will produce a link line
that does not work. `tests/pkgconfig.sh` checks that the flags are
expressible, not that they are complete.

**The sanitizer suites do not inherit the surrounding configuration.** [Low,
component: `Makefile.am`] `make check-asan` and its siblings configure a fresh
tree carrying over only `CC`, so a developer's `CFLAGS`, `--enable-werror` or
`--prefix` are absent from the instrumented run. That is deliberate, so that a
sanitizer run means one thing regardless of how the working tree was
configured, but it means the instrumented build is not the build being worked
on. Revisit if the difference ever hides something.

**The program-name check does not run in a libtool-wrapped build.** [Low,
component: `tests/version.sh`] `relic` rejects an `argv[0]` whose last
component is not printable ASCII, so a filename carrying a terminal escape
sequence cannot reach a diagnostic. Where the tool links an uninstalled
shared library, libtool wraps it in a script that re-executes the real binary
under a name of its own, which puts `argv[0]` out of reach; the test detects
that and skips. It runs under `--disable-shared` and against an installed
binary, so the property is covered, but not in the default build.

**The truly unterminated `strlcat` destination cannot be tested.** [Low,
component: `compat/strlcat.c`] The guard against a destination holding no
terminator is exercised with a buffer whose terminator lies beyond the size
passed to the call, and with a stated size of zero. A buffer with no
terminator anywhere is not a string, and AddressSanitizer's
`strict_string_checks` rejects the fixture before the function runs. The
arithmetic the guard protects is covered; the exact input a caller would pass
by mistake is not.

## Packaging

**`librelic` name collision.** [Low] An unrelated cryptographic toolkit
publishes `librelic` and a `relic.h`. Only matters if both install under one
prefix. Remedies are `--program-prefix` and a renamed pkg-config module.
Revisit if a distribution reports it.

## Post-1.0

Candidate features, in the priority order of `specs/architecture.md` section
69. None may change the language semantics of v1.

* CSV and TSV fact import
* arithmetic expressions
* incremental insertion maintenance
* DRed-style deletion maintenance
* aggregates
* parallel SCC evaluation
* persistent compiled rule plans
* relation export
* better cardinality estimation
* further Magic Sets and negation optimization
