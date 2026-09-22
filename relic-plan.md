# relic - Staged Implementation Plan

**Companion documents:** `specs/architecture.md` (target design), `specs/style.md`
(code style), `STATUS.md` (what exists), `TODO.md` (open work).

## 0. How to read this plan

Work is divided into fifteen stages, S0 through S14. Each stage is a single
reviewable change that ends with `make check` green and a commit. Six
milestones, M0 through M5, mark externally visible capability.

Each stage lists its goal, what it delivers, the design decisions it settles,
the tests it adds, and the condition under which it is done. "Budget" is new
non-comment production lines, plus or minus about thirty percent; it is a
planning aid and a pressure gauge, not a target to hit.

Three rules apply to every stage:

* no stage lands without its tests, and a bug fix lands as a failing test
  first;
* `make distcheck`, `make check-asan` and `make check-ubsan` must pass at every
  milestone;
* a subsystem gets its `specs/design-<name>.md` before its code, and the
  architecture document is updated only when a decision changes.

---

## 1. Cross-cutting decisions

These are settled once here so that no stage relitigates them. Each names its
escape hatch, because a decision with no way out is a bet rather than a
decision.

### D-1 Naming

The tool is `relic`, the library `librelic`, the public header `<relic.h>`, the
exported prefix `relic_` and `RELIC_`. Internal symbols with external linkage
also use `relic_` but are hidden; file-local symbols are `static`. Program
files conventionally end in `.dl`. The pkg-config module is `librelic`.

An unrelated cryptographic toolkit also publishes `librelic` and a `relic.h`.
relic installs its header directly as `$(includedir)/relic.h`, so the collision
only appears if both are installed under one prefix. Remedies are
`--program-prefix` and a renamed pkg-config module; neither touches the source
tree. Flagged once, not designed around.

### D-2 Language and dialect

ISO C11, compiled as `-std=gnu11` where available so BSD interfaces stay
visible on macOS, falling back to `-std=c11` and then to the compiler default.
No variable-length arrays, no `alloca`, no nested functions, no statement
expressions. `config.h` is included first in every `.c` file and never by
`include/relic.h`, which stays strict C11 and C++-includable.

### D-3 Tuple and value representation

**Decision: one uniform 32-bit interned domain.** Every value, integers
included, is interned into a single value table. A tuple is a dense
`relic_dom_t[arity]`, four bytes per column. Joins, hashing and duplicate
elimination are integer operations that never inspect a payload; only
comparison operators, sorting and output consult the table.

The three candidates, measured on two representative shapes: a symbol-heavy
graph (1M `edge/2` facts over 100k distinct symbols, the shape of dependency,
call and policy graphs) and an integer-heavy table (1M `size/2` facts with 1M
distinct integers).

| | A: 32-bit interned domain | B: tagged 64-bit cell | C: plain 16-byte value |
| --- | --- | --- | --- |
| Cell size | 4 bytes | 8 bytes | 16 bytes |
| Symbol-heavy tuples | 8 MB | 16 MB | 32 MB |
| Symbol-heavy values | ~3 MB | ~3 MB | ~3 MB |
| Integer-heavy tuples | 8 MB | 16 MB | 32 MB |
| Integer-heavy values | ~28 MB | 0 | 0 |
| Equality / join | `uint32` compare | `uint64` compare | type tag plus union |
| Tuple dedup | `bcmp` over `arity*4` | `bcmp` over `arity*8` | needs canonical padding |
| Hash input | dense words | dense words | struct with padding |
| Ordering `<` on int | table lookup | inline after untag | inline |
| Value table needed | yes, for all types | yes, for strings and boxed ints | yes, for strings |
| Distinct-value ceiling | 2^32 - 1 | 2^61 | none |
| Representation hazard | none | two encodings for one integer | padding and union bytes |

**Why A wins.** Symbol-valued columns dominate every workload in section 4 of
the architecture document; integer-heavy columns are the minority case, and A is
half the memory of B and a quarter of C exactly where the data actually is. A
is also the only option where a tuple is trivially `bcmp`-able and hashable as
a dense word array, which is the single hottest operation in the engine: every
insert does a full-tuple probe, and every join key does a partial one.

**What A costs.** The integer-heavy column is its weak case: roughly 24 bytes
plus a hash slot per *distinct* integer, so the second workload above costs
about 28 MB that B would not spend, plus one hash probe per integer at load
time. Repeated integers cost nothing extra, which is why the penalty lands on
distinct-integer columns specifically. There is also a discipline requirement:
domain identifiers are assigned in first-appearance order, so their numeric
order is meaningless and nothing may sort by them. That is one invariant,
asserted in one place, rather than a class of bug.

**Why not B.** The tag is not the problem; the boxing path is. Integers outside
the inline range must be boxed, so one integer has two possible encodings, and
set semantics require the choice to be canonical everywhere a value is
constructed - the parser, `relic_fact_add`, and any future arithmetic. A
non-canonical encoding does not crash; it silently makes `X = Y` false for
equal integers and quietly changes which tuples a relation contains. That is
exactly the "correctness rests on an enumeration of call sites" shape that
`CLAUDE.md` says to design out. B also doubles tuple memory in the common case
to win in the uncommon one.

**Why not C.** Storing `relic_value_t` directly means a tuple contains an enum,
padding and a union, so the fast paths cannot use `bcmp` or a word-wise hash
without first guaranteeing every padding byte and every unused union byte is
zeroed at every construction site. That is the same enumeration hazard as B,
for four times the memory and a branch per column in the join loop.

**Escape hatch.** `relic_dom_t` and about six accessors are the entire
interface to the representation: intern a value, look one up, compare two for
ordering, hash a tuple, compare two tuples, format one. Switching to B later
means rewriting those accessors and the tuple store, not the planner, the
evaluator, the Magic rewriter or the public API. The decision is reversible at
a bounded, known cost, which is the reason to take the simpler option now.

Revisit if a real workload shows integer-heavy columns dominating memory. The
`-X stats` counters report distinct values by type precisely so that this is a
measurement rather than an argument.

### D-4 Determinism

Fixed compile-time hash seeds, never randomized. Identifiers assigned in
first-appearance order. Relations iterate in insertion order. Planner ties
break by source position. Magic predicate names derive from predicate and
adornment, never a counter. No result and no `-e` output depends on a pointer
value. The golden test suite depends on all of this.

### D-5 Error model

Public functions return `0` or `-1`, or a pointer or `NULL`. Every failure
emits at least one `RELIC_ERROR` diagnostic through the context callback, and
`relic_error()` returns the last message. `errno` is never overloaded for
semantic errors and is preserved for system-call failures. The tool maps
everything to three exit statuses: 0 success, 1 program or query error, 2 usage.
`<sysexits.h>` may be used internally but does not reach the documented
interface.

### D-6 Memory

Four arenas by lifetime: context, program, query, evaluation. Arena allocation
returns `NULL` and sets a sticky per-arena failure flag; callers check the
pointer, and each phase confirms the flag at its boundary. Relation tuples and
indexes are not arena allocated: they outlive queries, they shrink, and they
are the allocations worth accounting against a limit. They use
`reallocarray()`. No `malloc()` per AST node or per tuple. The library never
aborts on allocation failure.

### D-7 Limits

Structural and fixed: arity at most 32, so an index key is a `uint32_t` column
bitmask; at most 64 distinct variables per rule, so a binding frame is a plain
array and a `uint64_t` bound-mask. Configurable and unlimited by default:
tuples, relation and index memory, generated Magic predicates, generated Magic
rules. Exceeding a limit is a diagnostic and a nonzero exit, and discards
partial results.

### D-8 The plan representation exists from S5

The executor is written once, in S5, against an ordered array of operations
(`SCAN`, `LOOKUP`, `MEMBER`, `FILTER`, `ANTI_MEMBER`, `EMIT`). S5 ships a
trivial planner that emits textual order and nothing but scans. Every later
optimization - indexes, access paths, body reordering, semi-naive delta
variants, Magic rewriting - changes what the planner emits, never how the
executor runs. This is what keeps S7, S9 and S10 from being rewrites, and it is
the single most important sequencing decision in this plan.

### D-9 The naive evaluator ships

The naive fixed point written in S5 stays in the library behind `-X naive` for
the life of the project, about 150 lines. It is the differential oracle. Both
evaluators share the parser, program model and relation store, so a
differential failure isolates the evaluator instead of implicating everything.
The alternative, a separate reference implementation in the test suite, would
duplicate the front end and test the duplicate rather than the product.

### D-10 Deltas are tuple identifier ranges

The tuple store is append-only within an evaluation, so the tuples derived in
one semi-naive iteration are exactly those with identifiers in a half-open
range. A delta needs no separate relation and no copying. The invariant this
rests on - no compaction, no deletion, no renumbering during a fixed point - is
asserted in the relation code and stated in `specs/design-eval.md`, because
future incremental maintenance is the change most likely to break it.

In delta rule variants the delta literal is pinned first in the plan. It is the
smallest input, and putting it first means the delta is only ever scanned, so
no index over a delta is ever needed.

### D-11 Comparison semantics

Equality is domain-identifier equality, which is exact because everything is
interned. Ordering of symbols and strings is bytewise, not locale collation.
Cross-type equality is false; cross-type ordering filters the binding out and
is reported once per rule at verbosity 2. `=` is a filter and does not bind.

### D-12 Generated names cannot collide

Internally generated predicates contain `$`, which the lexer rejects in an
identifier: `$q0` for a query, `path$bf` for an adorned predicate,
`magic$path$bf` for its magic relation. Collision with a user predicate is
impossible by construction rather than by a uniquifying counter, and the names
are still readable in `-e` output.

### D-13 Build

autoconf 2.69, automake 1.13, libtool 2.4.2 or newer, maintainer side only. A
release tarball builds with a C11 compiler, make and a POSIX shell. One
non-recursive `Makefile.am`. CI builds the tarball on every platform with no
autotools installed, because that is the path users take and it is the one that
breaks silently.

### D-14 Concurrency

A `relic_t` is not thread-safe. Distinct contexts share nothing: there is no
global mutable state, which S1 enforces with a link-time check that the
library's writable data and BSS sections are empty.

### D-15 Encoding

The lexer is byte-oriented and locale-independent: character classification
uses the project's own tables, never `<ctype.h>` with the locale. Identifiers
are ASCII. String literals hold arbitrary bytes, so UTF-8 passes through
unexamined, and output is byte-transparent.

### D-16 Integer literals

Parsed with `strtoll` into `int64_t`; out of range is a parse error naming the
literal. A `-` immediately preceding a digit is part of the literal token,
which is unambiguous because v1 has no arithmetic. `LLONG_MIN` is
representable.

---

## 2. Stage dependencies

```text
S0 build skeleton
 |
S1 foundations: arena, diag, intern, values
 |
 +-----------------+
 |                 |
S2 lexer+parser   S4 relations+tuple store
 |                 |
S3 program model   |
 +--------+--------+
          |
         S5 plan IR + naive executor + CLI          == M1
          |
         S6 dependency graph, SCC, stratification
          |
         S7 semi-naive                              == M2
          |
         S8 negation execution
          |
         S9 indexes, statistics, greedy planner, -e == M3
          |
        S10 Magic Sets                              == M4
          |
 +--------+--------+
 |                 |
S11 library ABI   S12 REPL and CLI completion
 +--------+--------+
          |
        S13 differential and fuzz hardening
          |
        S14 documentation, packaging, release       == M5
```

S2 and S4 are independent after S1 and can proceed in parallel. S11 and S12 are
independent after S10. Everything else is a chain, because each stage's tests
depend on the previous stage's capability.

---

## 3. Stage summary

| Stage | Title | Budget | Ends at |
| --- | --- | --- | --- |
| S0 | Build skeleton | 400 build | M0 |
| S1 | Foundations | 870 | |
| S2 | Lexer and parser | 880 | |
| S3 | Program model and safety | 600 | |
| S4 | Relations and tuple store | 600 | |
| S5 | Plan representation, executor, CLI | 1670 | M1 |
| S6 | Dependency graph, SCC, stratification | 500 | |
| S7 | Semi-naive evaluation | 350 | M2 |
| S8 | Negation execution | 150 | |
| S9 | Indexes, statistics, planner, explain | 1050 | M3 |
| S10 | Magic Sets | 800 | M4 |
| S11 | Library ABI stabilization | 250 | |
| S12 | REPL and CLI completion | 450 | |
| S13 | Differential and fuzz hardening | 500 test | |
| S14 | Documentation, packaging, release | docs | M5 |

Engine total lands near 6.5 KLOC against the 4 to 7 KLOC budget of architecture
section 2.2; the tool adds about 1 KLOC against its stated 400 to 600. See
section 8, risk R7, for the reconciliation and the levers.

---

## 4. Stages

### S0 - Build skeleton

**Goal.** A tree that configures, builds, installs, tests and distributes,
containing one program that prints its version. Everything afterwards is filling
it in.

**Delivers.** `configure.ac`, `Makefile.am`, `autogen.sh`, `m4/`, `build-aux/`,
`librelic.pc.in`, the directory skeleton, `include/relic.h` with the version
macros and `relic_version()`, `cmd/relic/main.c` handling `-V` only, `LICENSE`,
`README.md`, `STATUS.md`, `TODO.md`, and CI configuration.

`configure.ac` outline:

```m4
AC_PREREQ([2.69])
AC_INIT([relic], [0.1.0], [relic-bugs@example.org], [relic])
AC_CONFIG_SRCDIR([lib/api.c])
AC_CONFIG_AUXDIR([build-aux])
AC_CONFIG_MACRO_DIRS([m4])
AC_CONFIG_HEADERS([config.h])
AC_CONFIG_LIBOBJ_DIR([compat])
AM_INIT_AUTOMAKE([1.13 foreign subdir-objects -Wall -Werror])
AM_SILENT_RULES([yes])

AC_USE_SYSTEM_EXTENSIONS
RELIC_PROG_CC_C11              dnl m4/relic_c11.m4: gnu11, then c11, then default
AM_PROG_AR
LT_INIT

RELIC_ABI_CURRENT=0
RELIC_ABI_REVISION=0
RELIC_ABI_AGE=0
AC_SUBST([RELIC_ABI_CURRENT])
AC_SUBST([RELIC_ABI_REVISION])
AC_SUBST([RELIC_ABI_AGE])

AC_CHECK_HEADERS([err.h sysexits.h histedit.h])
AC_CHECK_FUNCS([getprogname])
AC_REPLACE_FUNCS([reallocarray strlcat strlcpy strtonum])

RELIC_ARG_SANITIZERS            dnl m4/relic_sanitizers.m4: asan, ubsan, msan
RELIC_ARG_WITH_LIBEDIT          dnl m4/relic_libedit.m4: auto, yes, no
RELIC_CHECK_CFLAGS([-Wall -Wextra -Wshadow -Wpointer-arith -Wwrite-strings ...])

AC_CONFIG_FILES([Makefile librelic.pc])
AC_OUTPUT
```

`Makefile.am` outline:

```make
ACLOCAL_AMFLAGS = -I m4
AM_CPPFLAGS = -I$(top_srcdir)/include -I$(top_srcdir)/lib \
	      -I$(top_srcdir)/compat -I$(top_builddir)
AM_CFLAGS = $(RELIC_WARN_CFLAGS) $(RELIC_SAN_CFLAGS)
LIBOBJDIR = compat/

lib_LTLIBRARIES = librelic.la
librelic_la_SOURCES = lib/api.c ...
librelic_la_LIBADD = $(LTLIBOBJS)
librelic_la_LDFLAGS = -no-undefined -export-symbols-regex '^relic_' \
	-version-info $(RELIC_ABI_CURRENT):$(RELIC_ABI_REVISION):$(RELIC_ABI_AGE)
include_HEADERS = include/relic.h

bin_PROGRAMS = relic
relic_SOURCES = cmd/relic/main.c cmd/relic/cmd.h
relic_LDADD = librelic.la $(LIBEDIT_LIBS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = librelic.pc
dist_man1_MANS = man/relic.1
dist_man3_MANS = man/relic.3

TEST_EXTENSIONS = .dl .sh
DL_LOG_COMPILER = $(top_srcdir)/tests/langtest.sh
AM_TESTS_ENVIRONMENT = RELIC='$(abs_top_builddir)/relic$(EXEEXT)'; export RELIC;
check_PROGRAMS =
TESTS = $(check_PROGRAMS)
EXTRA_DIST = autogen.sh tests/langtest.sh
```

**Design notes.** Local m4 macros rather than autoconf-archive, so a maintainer
needs no extra package. `LIBOBJDIR` is required for `AC_REPLACE_FUNCS` objects
to land correctly in a non-recursive build with `subdir-objects`. `-no-undefined`
from the start, because it is what makes a shared build fail loudly rather than
quietly on platforms that require complete link closure.

The sanitizer targets are real from day one, not added later: a tree that has
never been built under ASan accumulates findings faster than it can absorb them.

**Tests.** A smoke test that `relic -V` prints the version and exits 0; a test
that the tree contains no byte above 0x7f in a source file; a test that
`librelic.pc` resolves.

**Done when.** On macOS and Linux: `./autogen.sh && ./configure && make &&
make check && make distcheck` all succeed, in-tree and out-of-tree; the
resulting tarball builds on a machine with no autotools; `make check-asan` and
`make check-ubsan` run and pass. **M0.**

**Budget.** About 400 lines of build machinery, 80 lines of C.

---

### S1 - Foundations

**Goal.** The primitives every later stage sits on, with no Datalog in them.

**Delivers.** `compat/` per architecture section 46. `lib/arena.c` with chunked
regions and the sticky-failure contract of D-6. `lib/diag.c` with
`relic_location_t`, the diagnostic callback, the default `warnx`-style
formatter used by the tool, and the last-error buffer. `lib/util.h` with the
growable-array helpers over `reallocarray()` and the open-addressing hash
primitives. `lib/symbol.c` interning byte strings into a string pool.
`lib/value.c` holding the value table and `relic_dom_t` per D-3.
`specs/design-memory.md`.

**Design notes.** One hash implementation, parameterized by what it stores, is
used by the symbol table, the value table, the full-tuple index and later the
secondary indexes. Four separate open-addressing tables would be four sets of
probe-sequence bugs. It stores 32-bit identifiers inline, which is the whole
reason not to reach for a generic container.

Interning of integers happens here, not in the parser, so `relic_fact_add()`
and the parser cannot diverge on canonical form.

**Tests.** Unit tests per module: arena growth and the failure flag; interning
returning identical identifiers for equal bytes and distinct ones for unequal;
embedded NUL bytes in strings; the value table round-tripping every type
including `LLONG_MIN`; hash table growth, deletion if supported, and behaviour
at full load; a test that the library has no writable global state.

**Done when.** Unit tests pass under ASan and UBSan with no leaks.

**Budget.** 870.

---

### S2 - Lexer and parser

**Goal.** Source text becomes an AST, and bad source text becomes good
diagnostics.

**Delivers.** `lib/lex.c`, `lib/parse.c`, `lib/ast.h`, the first fuzz target,
`specs/design-parser.md`.

**Design notes.** Hand-written recursive descent over the grammar of
architecture section 13. Every token carries a `relic_location_t` with line and
column; the column is what makes a diagnostic worth reading.

Error recovery synchronizes on `.` and `?`, so one malformed rule does not
produce a cascade and a file with several mistakes reports all of them in one
run. That behaviour is specified by golden tests, because error recovery that
is not tested silently degrades.

The lexer classifies bytes from its own table per D-15. Strings support `\\`,
`\"`, `\n`, `\t`, `\r`, `\0` and `\xHH`; an unknown escape is an error rather
than a silent literal, since the permissive reading cannot be taken back later.

**Tests.** Golden tests for every construct in section 13; every token class at
a buffer boundary; unterminated string, unterminated comment at end of file,
integer overflow, `%` comment at end of file without a newline, zero-arity
predicate, empty file, file of only comments; multi-error recovery; a fuzz
target reaching no-crash status on a seed corpus.

**Done when.** `relic -c` parses the architecture document's examples and
reports the golden diagnostics for the malformed corpus.

**Budget.** 880.

---

### S3 - Program model and safety

**Goal.** An AST becomes a validated program, and `relic -c` becomes useful.

**Delivers.** `lib/pred.c` with the `(name, arity)` predicate table,
`lib/rule.c` with variable numbering and the safety analysis, the arity and
variable-count limits of D-7, `specs/design-program.md`.

**Design notes.** Safety reduces to one set computation: let *P* be the
variables occurring in positive relational body literals. A rule is safe when
the head variables, the variables of every negated literal and the variables of
every comparison are all in *P*. No reordering search is needed, because all
positive literals can always be scheduled before all negated literals and
comparisons, so if a binding is possible at all it is possible in that order.
Stating the check this way is what keeps it five lines instead of a fixed point
over literal orderings.

Diagnostics name the variable and the rule and point at the column, matching
the form in architecture section 30.

Warnings, none of them fatal: singleton variables per architecture section 7; a
predicate referenced but never defined, reported once, which is a typo far more
often than an intentionally empty relation.

**Tests.** The accepted and rejected examples of architecture section 14;
arity and variable limits at the boundary; a predicate with both facts and
rules; `foo/1` and `foo/2` staying distinct; singleton warnings including the
`_`-prefixed exemption; golden stderr for each diagnostic.

**Done when.** `relic -c` accepts and rejects the architecture section 72
program correctly and every diagnostic has a golden test.

**Budget.** 600.

---

### S4 - Relations and tuple store

**Goal.** Somewhere to put tuples, with the insert-returns-newness property
that fixed-point detection is built on.

**Delivers.** `lib/relation.c`: the dense tuple array, the canonical full-tuple
hash index, `relic_relation_insert()` returning true only on a genuinely new
tuple, iteration by identifier range, and the per-relation counters that S9's
statistics will extend. `specs/design-relations.md`.

**Design notes.** Arity zero is a relation holding zero or one tuples and is
handled by the general code path with an empty key, not by a special case; it
falls out of the representation if the hash of an empty key is defined.

The append-only invariant of D-10 is asserted here, and this is where a future
incremental-maintenance change will have to confront it.

Memory accounting for the `-l memory` limit is hooked in at this layer, since
tuples and indexes are what actually grow.

**Tests.** Insert, reinsert, membership; duplicate facts collapsing; identifier
ranges after a series of inserts; arity 0 and arity 32; a one-million-tuple
insert that stays within an expected memory envelope; the memory limit firing
with the specified diagnostic and leaving the relation consistent.

**Done when.** Unit tests pass under ASan, and the million-tuple case runs in
the CI time budget.

**Budget.** 600.

---

### S5 - Plan representation, executor, CLI  (M1)

**Goal.** End to end. Positive recursive Datalog answers queries from the
command line.

**Delivers.** `lib/plan.c` with the operation array of D-8 and a deliberately
trivial planner: textual order, `SCAN` for every positive literal, `MEMBER`
when every argument is already bound, `FILTER` for comparisons. `lib/eval.c`
with the executor over that array and the naive fixed point of D-9. Query
compilation into the `$q0` synthetic predicate of D-12, with output columns in
first-occurrence order and anonymous variables omitted. `cmd/relic/main.c` with
full option parsing, `cmd/relic/output.c` with text, csv, tsv and json, and
`-s`. `specs/design-plan.md` and `specs/design-eval.md`.

**Design notes.** The executor is written once and never rewritten; everything
later changes the plan it is handed. It is a loop over the operation array with
an explicit binding frame, not recursion over literals, so rule depth cannot
overflow the C stack.

The json format is fixed and documented now, because output formats are
interfaces: an array of row arrays, values as json strings for symbols and
strings and as json numbers for integers, with symbols and strings
distinguished by a documented convention rather than left ambiguous.

`-s` uses the total order of architecture section 61, which is the first place
mixed-type columns become visible.

**Tests.** The architecture section 72 graph program; recursion, mutual
recursion, cyclic graphs, multiple recursive atoms; every query binding
pattern; duplicate facts; all four output formats against golden files; `-o`;
stdin when not a terminal; exit statuses 0, 1 and 2; `-q` repeated.

**Done when.** `relic -q 'path(a, X)?' graph.dl` prints `b c d`, `relic -a`
materializes, and every output format has a golden test. **M1.**

**Budget.** 1670, of which about 600 is the tool.

---

### S6 - Dependency graph, SCC, stratification

**Goal.** Know what depends on what, in which order it must be evaluated, and
which programs to reject.

**Delivers.** `lib/depgraph.c` building the predicate graph with positive and
negative edges, iterative Tarjan SCC, and the condensation in topological
order. `lib/stratify.c` assigning strata and rejecting a negative edge inside
an SCC. The evaluation driver becomes: strata in order, SCCs within a stratum
in topological order. `specs/design-stratify.md`.

**Design notes.** Tarjan is iterative with an explicit stack. A recursive
implementation is shorter and is a stack-overflow bug waiting for a generated
program with ten thousand predicates, which the S13 generator will produce.

The unstratified diagnostic must name the cycle, in the form of architecture
section 30: the error at the rule that closes the cycle, then one note per edge
with its source location. A bare "program is not stratified" would be
technically correct and useless, so the notes are specified by golden tests.

Negation is still rejected at evaluation here; only the analysis exists. This
split keeps S6 reviewable as a graph change and S8 reviewable as an evaluator
change.

**Tests.** Single predicate, chain, diamond, self-loop, mutual recursion, two
independent SCCs in one stratum; deterministic SCC order for isomorphic inputs
presented in different textual order; the architecture section 9 unstratified
example rejected with the exact golden diagnostic; a ten-thousand-predicate
chain that does not overflow.

**Done when.** Stratification golden tests pass and the naive evaluator now
runs stratum by stratum with identical results to S5.

**Budget.** 500.

---

### S7 - Semi-naive evaluation  (M2)

**Goal.** Recursion stops redoing work it has already done.

**Delivers.** Delta rule variant generation in `lib/plan.c`, the fixed-point
loop over identifier ranges per D-10 in `lib/eval.c`, and the counters that
`-X stats` reports.

**Design notes.** For a rule with *k* recursive body literals, generate *k*
variants; variant *i* binds literal *i* to the delta and the rest to the full
relation. This is the scheme of architecture section 28. It derives some tuples
more than once within an iteration, which the destination relation discards.
The alternative that eliminates those duplicates - old for earlier literals,
new for later ones - is a known refinement, is not obviously correct on first
reading, and saves work only for rules with three or more recursive literals,
which are rare. Note it in `TODO.md` as a measured optimization rather than
guessing now.

The delta literal is pinned first in the plan per D-10, so a delta is only ever
scanned and no delta index is needed.

Plans are built once per rule variant when its SCC's fixed point starts, not
per iteration. Replanning each iteration would chase changing cardinalities at
a cost that is hard to bound and would make `-e` output depend on iteration
count.

**Tests.** Every recursive test from S5 and S6 rerun under both evaluators and
compared, which is the first use of the equivalence harness. Transitive closure
over a chain, a cycle, a complete graph and a disconnected graph; the
`p(X,Z) :- p(X,Y), p(Y,Z)` double-recursive case; iteration counts asserted so
a regression to naive behaviour fails rather than merely slows.

**Done when.** `semi-naive(P) == naive(P)` over the whole corpus, and the
iteration-count assertions hold. **M2.**

**Budget.** 350.

---

### S8 - Negation execution

**Goal.** `not` works.

**Delivers.** The `ANTI_MEMBER` operation in the executor, planner support for
placing a negated literal once its variables are bound, and the safety
interaction with S3.

**Design notes.** Safety already guarantees every variable of a negated literal
is bound by some positive literal, so the planner schedules negated literals
after those, and the operation is a full-tuple membership test that inverts its
result. Stratification already guarantees the negated relation is complete when
the test runs. Negation therefore adds one operation and no new machinery,
which is the payoff for having done S3 and S6 first.

**Tests.** The `blocked`/`ready` program of architecture section 72; negation
over an empty relation; negation over a relation completed in an earlier
stratum; a negated literal with constants; double negation; and the
already-rejected unstratified cases reconfirmed.

**Done when.** The full architecture section 72 program produces correct
results under both evaluators.

**Budget.** 150.

---

### S9 - Indexes, statistics, planner, explain  (M3)

**Goal.** Joins stop being nested scans, and the chosen plan becomes visible.

**Delivers.** `lib/index.c` with column-bitmask-keyed hash indexes over tuple
identifiers, built lazily and extended incrementally on insert. Access path
selection: given the bound mask, pick the index whose key mask is contained in
it and maximal, else scan. Greedy body ordering. Relation statistics. `-e`.
`specs/design-index.md`, `specs/design-planner.md`.

**Design notes.** An index is keyed by a `uint32_t` column bitmask, which is
why arity is capped at 32 in D-7. Indexes are extended on insert rather than
invalidated, so a fixed point does not rebuild them every iteration.

The greedy cost model scores a literal by bound-argument count, then estimated
selectivity, then index availability, and breaks ties by source position per
D-4. Per-column distinct counts are computed on demand and cached against a
dirty flag, rather than maintained per insert: planning happens once per SCC
and insertion happens millions of times.

The planner needs a second mode for S10: at Magic rewrite time the IDB
relations are empty, so cardinality must come from a static estimate. Building
that mode now, with EDB counts real and IDB counts a documented constant, is
what stops S10 from growing its own private notion of body ordering, which
architecture section 35 explicitly forbids.

`-e` prints the adornment, the strata, the SCCs, the plan per rule and the
delta variants, in the format of architecture section 60.

**Tests.** `indexed(P) == no-index(P)` and `reordered(P) == textual-order(P)`
across the corpus, via `-X`. Access path selection for every binding pattern of
a 3-ary predicate. Index correctness under insertion during a fixed point.
Deterministic, byte-identical `-e` output across runs. A performance smoke test:
reachability over a one-million-edge graph within a CI time budget, failing if
it regresses.

**Done when.** Both equivalences hold, `-e` has golden tests, and the
performance smoke test passes. **M3.**

**Budget.** 1050.

---

### S10 - Magic Sets  (M4)

**Goal.** A bound query stops computing the whole relation.

**Delivers.** `lib/magic.c`: the adornment worklist memoized on
`(predicate, arity, binding pattern)`, SIPS driven by the S9 planner in static
mode, magic seed and magic propagation rule generation, structural
deduplication of generated rules, and the D-7 limits on generated predicates
and rules. The rewritten program is then stratified and evaluated by the
existing engine, with no second execution path. `specs/design-magic.md`.

**Design notes.** This is the highest-risk stage in the plan and the one to
schedule with the most slack.

Magic rewriting is a program-to-program transformation into the query arena.
The rewritten program goes through S6's stratification unchanged, which is both
a correctness check and a guard: if a rewrite ever produces an unstratified
program, the existing analysis catches it rather than the evaluator producing
nonsense.

Propagation follows positive edges only. A negatively referenced relation is
materialized completely in its lower stratum, per architecture section 37. This
is conservative and is the reason stratified negation and Magic Sets can
coexist without either becoming intricate.

Adornment generation terminates because there are at most `2^arity` adornments
per predicate and the key is memoized; with arity capped at 32 by D-7 this is
finite but not small, so the generated-predicate limit is the practical bound
and its diagnostic must say which predicate exploded.

**Tests.** `magic(Q, P) == Q(full_materialize(P))` over the corpus and over
generated programs - the central equivalence of the project. Selectivity
assertions: `path(a, X)?` over a large graph must derive far fewer tuples than
full materialization, asserted on the `-X stats` counter so that a rewrite that
is correct but ineffective still fails. Adornment memoization producing a
finite rule set for a mutually recursive program. `-e` showing seed and
propagation rules.

**Done when.** The equivalence holds everywhere, the selectivity assertions
pass, and `relic -e -q 'path(a, X)?' graph.dl` reproduces architecture section
60. **M4.**

**Budget.** 800.

---

### S11 - Library ABI stabilization

**Goal.** `librelic` becomes something another program can depend on.

**Delivers.** The final `include/relic.h`: lifecycle, configuration, loading,
`relic_check()`, prepared queries, the streaming row callback, the collector
built on top of it, `relic_materialize()`, `relic_fact_add()`, the diagnostic
callback and `relic_error()`. Hidden visibility plus the export regex.
`examples/embed.c`. `man/relic.3`. ABI version set to `1:0:0`.

**Design notes.** The tool has been a client of the library since S5, so this
stage freezes and documents an interface that is already in use rather than
inventing one. Anything the tool needed that is not exported is a finding: it
means either the interface is incomplete or the tool reached inside.

`relic_fact_add()` invalidates derived state per architecture section 54. The
invalidation is total in v1 - derived relations are discarded and the next
query rematerializes - and it is specified that way rather than left to
whatever the implementation happens to do.

**Tests.** `examples/embed.c` built against the *installed* library and
pkg-config output, not against the build tree, which is the only way to catch a
missing export or a header that needs an internal include. A symbol audit
asserting the dynamic symbol table contains only `relic_*`. Streaming callback
early termination. Diagnostic callback receiving every diagnostic class with no
stderr output. `relic_fact_add()` after materialization.

**Done when.** The example builds and runs against the installed library on
every CI platform, and the symbol audit passes.

**Budget.** 250.

---

### S12 - REPL and CLI completion

**Goal.** The tool is finished.

**Delivers.** `cmd/relic/repl.c` with `-i`, the `.help`, `.stats`, `.plan` and
`.quit` commands, optional libedit, the `-l` limit parsing of D-7, `-n`,
verbosity levels, and `man/relic.1`.

**Design notes.** libedit is detected by `configure` and its absence changes
only input handling; the two paths meet at one function that returns the next
line, so the REPL logic exists once.

`-l` values are validated for range and trailing characters with `strtonum`,
per `specs/style.md`.

**Tests.** REPL driven from a pipe with a scripted session compared against a
golden transcript, which works precisely because the REPL is not required to be
interactive. Each dot command. Unknown dot command. EOF mid-query. Every `-l`
limit parsed, rejected when malformed, and enforced. Manual page linted with
`mandoc -Tlint`.

**Done when.** The golden REPL transcript passes with and without libedit, and
`mandoc -Tlint` is clean.

**Budget.** 450.

---

### S13 - Differential and fuzz hardening

**Goal.** Find the bugs the curated corpus cannot.

**Delivers.** `tests/random/` with a seeded generator of safe, stratified
programs and a shrinker that reduces a failing program to a minimal one.
`tests/fuzz/` with parser and evaluator targets. `bench/` with generated
workloads and a timing harness. `make check-random` and `make coverage`.

**Design notes.** The generator produces programs, not strings: it builds a
valid AST and prints it, so it spends its time on evaluator semantics rather
than rediscovering the parser. It varies arity, recursion depth, SCC shape,
negation stratification, comparison types and mixed-type columns, and it emits
the seed so every failure is reproducible.

The shrinker matters more than the generator. A hundred-rule counterexample is
a curiosity; the five-rule reduction of it is a bug report, and a reduction
found by hand at three in the morning is a bug report nobody writes.

Coverage is a gap-finder, not a target. The action on a coverage report is to
name the uncovered branch and either test it or delete it.

**Tests.** All four equivalences of architecture section 66.2 over generated
programs. Fuzz targets reaching a no-crash, no-hang, no-leak state over a
corpus. Coverage reported per module with every gap either covered or noted in
`TODO.md`.

**Done when.** `make check-random` passes a thousand seeds clean, both fuzz
targets survive an extended run under ASan and UBSan, and the coverage gaps are
enumerated.

**Budget.** 500 lines of test code.

---

### S14 - Documentation, packaging, release  (M5)

**Goal.** relic 1.0.

**Delivers.** Complete `relic.1` and `relic.3`, a `README.md` that gets someone
from tarball to first query, `NEWS`, the 2-clause BSD `LICENSE`, embedding
documentation, `contrib/` skeletons for FreeBSD ports, OpenBSD ports, pkgsrc
and Homebrew, and the release tarball.

**Design notes.** The manual pages are mdoc and are linted in CI. They are the
interface documentation; the architecture document is design documentation, and
neither substitutes for the other.

`STATUS.md` becomes accurate as of the release, and `TODO.md` becomes the 1.1
agenda.

**Tests.** Every command line in the manual page and the README executed by a
test, because documented examples that were never run are the most reliably
wrong text in any project. `make distcheck` on every platform. An install,
`pkg-config` query and example build from a clean prefix.

**Done when.** Every criterion in architecture section 72 is demonstrated by a
test that runs in CI. **M5.**

---

## 5. Test architecture

```text
tests/unit/       one C binary per module; plain assertions; fast
tests/lang/       *.dl with *.expected, *.expected-err, *.exit, *.args
tests/equiv/      the four equivalences, driven through -X
tests/random/     generator, shrinker, seed corpus
tests/fuzz/       libFuzzer entry points
bench/            generated workloads, timing harness, recorded baselines
```

A language test is a file and its expectations, with no code:

```text
tests/lang/path-bound.dl           the program, ending in a query
tests/lang/path-bound.args         optional: -s -F csv
tests/lang/path-bound.expected     expected stdout
tests/lang/path-bound.expected-err optional: expected stderr
tests/lang/path-bound.exit         optional: expected exit status, default 0
```

`tests/langtest.sh` is registered as automake's `DL_LOG_COMPILER`, so every
`.dl` file under `tests/lang/` is a test with no per-test registration and no
way to add a test that is silently not run.

Diagnostics are golden-tested from S2 onward. Error message quality is a
feature of this tool, and an untested diagnostic degrades one refactor at a
time.

---

## 6. Continuous integration

| Job | Purpose |
| --- | --- |
| Linux gcc, glibc | primary, `--enable-werror` |
| Linux clang | second front end's warnings |
| Linux clang, ASan + UBSan | `make check-asan`, `make check-ubsan` |
| Linux clang, MSan | uninitialized reads ASan cannot see |
| Linux gcc, valgrind | a third opinion on memory |
| Alpine, musl | exercises the `compat/` paths |
| Linux gcc `-m32` | catches `size_t` and pointer-width assumptions |
| macOS clang | the development platform |
| FreeBSD, OpenBSD, NetBSD | the primary target platforms |
| tarball | `make dist` on Linux, then build it everywhere with no autotools |
| distcheck | out-of-tree, `DESTDIR`, `--disable-shared`, `--disable-static` |
| ascii | no byte above 0x7f in a source file |
| mandoc | `-Tlint` on both manual pages |

The tarball job is the one most likely to catch a real user-visible break,
because it is the only job that builds what users actually download.

---

## 7. Traceability

| Architecture sections | Stage |
| --- | --- |
| 64, 65, 46 | S0 |
| 18, 19, 47, 56, 57 | S1 |
| 5-13 | S2 |
| 14, 17, 25, 63 | S3 |
| 20, 21 | S4 |
| 11, 15, 26, 39, 43, 44, 58, 59 | S5 |
| 29, 30 | S6 |
| 27, 28 | S7 |
| 9, 31 | S8 |
| 22, 23, 24, 60, 61, 62 | S9 |
| 32-38 | S10 |
| 48-57 | S11 |
| 40, 41, 42 | S12 |
| 66, 67 | S13 |
| 65.5, 72, 74, 75 | S14 |

---

## 8. Risks

**R1 - Magic Sets correctness. High impact, high likelihood.** The rewrite is
where subtle wrongness hides, and a wrong answer set looks exactly like a right
one. Mitigated by the equivalence against full materialization, by running the
rewritten program through the unchanged stratification analysis, by generated
programs in S13, and by scheduling S10 with slack. If S10 overruns, v1 can ship
with `-X no-magic` as the default and Magic Sets as opt-in; the engine is
correct without it, only slower on bound queries.

**R2 - SIPS quality without cardinalities.** At rewrite time IDB relations are
empty, so the ordering that drives magic propagation is a guess. Mitigated by
making the guess explicit and inspectable through `-e` rather than implicit,
and by sharing one planner between SIPS and execution per architecture section
35. A bad SIPS choice costs performance, not correctness.

**R3 - The append-only invariant.** D-10 makes deltas identifier ranges, which
is elegant and is silently wrong the moment anything compacts, deletes or
renumbers a relation. Mitigated by asserting it in `lib/relation.c`, stating it
in `specs/design-eval.md`, and naming it in `TODO.md` as the first thing
incremental maintenance must address.

**R4 - Memory on unbounded closures.** A transitive closure over a dense graph
is quadratic and users will write one. Mitigated by the limits of D-7 defaulting
to unlimited but being easy to set, and by a diagnostic that names the limit and
the relation being derived rather than dying in the allocator.

**R5 - Autotools portability.** Version skew across the BSDs breaks
regeneration, not building. Mitigated by requiring only autoconf 2.69, by
avoiding autoconf-archive in favour of local m4, and by CI building the tarball
on every platform with no autotools installed.

**R6 - Symbol visibility.** `-export-symbols-regex` and hidden visibility
behave differently across platforms and can silently export everything.
Mitigated by the S11 symbol audit running in CI on every platform.

**R7 - Size budget.** This plan totals about 7.5 KLOC against the 4 to 7 KLOC
of architecture section 2.2, roughly 15 percent over, concentrated in S9's
indexing and S10's rewriting. Since section 2.2 makes size a design constraint
rather than a statistic, the overrun is tracked rather than accepted silently.
The levers, in the order they should be pulled: merge `lib/pred.c` into
`lib/rule.c` and `lib/stratify.c` into `lib/depgraph.c`, which architecture
section 64 already permits; drop one output format if json and csv prove
redundant; and, last, reduce the index implementation to a single key shape.
Cutting Magic Sets is not a lever - it is the feature.

**R8 - Scope creep from post-v1 features.** Arithmetic, aggregates and
incremental maintenance are each individually reasonable and collectively a
different product. Mitigated by architecture section 69 and by `TODO.md`: an
idea gets written down, not implemented.

---

## 9. What happens after each stage

Every stage ends the same way: design document if the stage introduces a
subsystem, code, tests, a code review pass per `CLAUDE.md` covering bugs,
design, security and readability, `make check` plus `make check-asan` and
`make check-ubsan`, then `STATUS.md` and `TODO.md` updated and a commit.

The architecture document is not updated at the end of a stage. It is updated
when a decision in it changes.
