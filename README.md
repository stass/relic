# relic

relic is a small, general-purpose Datalog engine for C: a library, `librelic`,
and a command-line tool, `relic`, that loads a Datalog program, evaluates it
bottom-up and answers queries against it.

It has no third-party runtime dependencies. The engine and the tool use the
C11 standard library and a handful of BSD interfaces, with a replacement
compiled from `compat/` on a platform that lacks one. The single optional
dependency is `libedit`, used only for line editing in interactive mode.

## Building

From a release tarball, nothing beyond a C11 compiler, `make` and a POSIX
shell is required:

```sh
./configure
make
make check
make install
```

Out-of-tree builds are supported and preferred:

```sh
mkdir build && cd build
../configure && make && make check
```

From a git checkout, regenerate the build system first. This needs autoconf
2.69, automake 1.13 and libtool 2.4.2 or newer, which are maintainer tools
only:

```sh
./autogen.sh
```

### Build options

```text
--enable-debug          assertions, -O0 -g3                    (default: no)
--enable-werror         treat warnings as errors               (default: no)
--disable-threads       evaluate on the calling thread alone   (default: threads)
--enable-asan           AddressSanitizer                       (default: no)
--enable-ubsan          UndefinedBehaviorSanitizer             (default: no)
--enable-lsan           LeakSanitizer, where it stands alone   (default: no)
--enable-msan           MemorySanitizer, clang on Linux        (default: no)
--enable-tsan           ThreadSanitizer                        (default: no)
--enable-coverage       gcov or llvm-cov instrumentation       (default: no)
--enable-fuzzers        build tests/fuzz, clang only           (default: no)
--with-libedit=auto|yes|no
                        line editing and history in the REPL   (default: auto)
```

The standard autotools options apply unchanged, including `--prefix`,
`--program-prefix`, `--disable-shared`, `--disable-static` and `DESTDIR`
staged installation.

### Test suites

```sh
make check          the unit and language suites
make check-asan     reconfigure with AddressSanitizer, rebuild, run the suite
make check-lsan     the same under LeakSanitizer, where it stands alone
make check-msan     the same under MemorySanitizer, where it is supported
make check-ubsan    the same under UndefinedBehaviorSanitizer
make check-tsan     the same under ThreadSanitizer, with threads enabled
```

Each sanitizer suite configures a disposable build tree under `disposable/`
in the current build directory, from a copy of the distributed sources, so it
never leaves instrumented objects in the tree you work in and works whether
that tree was configured in place or not. A sanitizer the platform does not
support reports that and succeeds rather than failing the build.

A few tests need a tool the build does not, such as `mandoc`, `nm` or
`pkg-config`, and are skipped without it. On a machine set up to run every
check, set `RELIC_STRICT_CHECK=yes` in the environment: a skipped test then
fails, and so do a sanitizer suite the compiler cannot build and an
`installcheck` without `pkg-config`. A skip that is expected there is named
in `RELIC_ALLOWED_SKIPS`, a space-separated list of test file names and
targets:

```sh
RELIC_STRICT_CHECK=yes RELIC_ALLOWED_SKIPS=symbols.sh make distcheck
```

## A first query

A program is a sequence of facts, rules and queries. This one states two
edges of a graph and derives the paths they form:

```text graph.dl
% A small graph and its transitive closure.
edge(a, b).
edge(b, c).
path(X, Y) :- edge(X, Y).
path(X, Z) :- edge(X, Y), path(Y, Z).
```

A query names the rows it wants, with variables where any value will do:

```sh
$ relic -q 'path(a, X)?' graph.dl
b
c
```

A query can also be written in the program itself, ending in `?` as above,
and `relic -i graph.dl` answers queries typed one per line. `relic(1)`
describes the language, the options, the import of facts from CSV and TSV
tables, and the export of relations.

## Data files

A program cannot name a file: relic has no counterpart of Souffle's `.input`
and `.output` directives or Nemo's `@import`. A program may come from someone
else, and loading one gives it no capability but computation, so it cannot
read a file its runner did not name, and it means the same thing on every
machine it is loaded on. Whoever runs the tool names the data instead. `-I`
imports a CSV or TSV table as the facts of a predicate, named by its file
name, and `-T` gives the predicate's column types; here `edge.tsv` holds
the two edges of `graph.dl` and `reach.dl` its rules and query:

```sh
$ relic -T edge:symbol,symbol -I edge.tsv reach.dl
b
c
```

`-F` and `-o` choose the format of a query's answer and the file it is
written to. `-O` writes a whole relation to a file named the same way, once
the queries have run: `-O path.tsv` as a table that `-T` and `-I` read back,
and `-O path.dl` as facts that a later run loads as a program, every value
keeping its type:

```sh
$ relic -O path.dl graph.dl && relic -q 'path(a, X)?' path.dl
b
c
```

## Embedding

`librelic` installs a single public header, `<relic.h>`, and a pkg-config
module, `librelic`. `examples/query.c` is a complete program that loads a
program, answers a query and prints the rows, and is the example `relic(3)`
shows:

```sh
cc -o query examples/query.c $(pkg-config --cflags --libs librelic)
./query
```

Only symbols matching `relic_` are exported. `relic(3)` describes the
interface.

## Limitations

A query that binds an argument is answered through a Magic Sets rewrite,
which derives only what the query can reach. A recursive rule that calls
its own predicate twice, with the bound argument passed to the first call,
costs that rewrite a second relation as large as the closure it derives. Of
these two definitions of a path, called as `path(a, X)?`:

```prolog
path(X, Z) :- path(X, Y), path(Y, Z).
path(X, Z) :- path(X, Y), edge(Y, Z).
```

the first keeps a second relation holding a copy of every path the rewrite
derives, and the second does not. Both give the same answers, and the copy
counts against the memory limit like any other relation. `relic -e` shows
the copy as a predicate named `sup$...`. Where memory matters, prefer the
linear form.

## Documentation

| File | Contents |
| --- | --- |
| [man/relic.1](man/relic.1) | the tool and the language |
| [man/relic.3](man/relic.3) | the library |
| [specs/architecture.md](specs/architecture.md) | the target design of the system |
| [specs/style.md](specs/style.md) | code style, binding on all relic-owned code |
| [specs/decisions.md](specs/decisions.md) | the numbered decisions the design rests on |
| [NEWS](NEWS) | what changed in each release |
| [TODO.md](TODO.md) | open work, by severity |

## License

BSD 2-Clause. See [LICENSE](LICENSE).
