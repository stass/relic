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
describes the language, the options, and the import of facts from CSV and
TSV tables.

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

## Documentation

| File | Contents |
| --- | --- |
| [man/relic.1](man/relic.1) | the tool and the language |
| [man/relic.3](man/relic.3) | the library |
| [specs/architecture.md](specs/architecture.md) | the target design of the system |
| [specs/style.md](specs/style.md) | code style, binding on all relic-owned code |
| [relic-plan.md](relic-plan.md) | the staged implementation plan and decision record |
| [STATUS.md](STATUS.md) | what exists today |
| [TODO.md](TODO.md) | open work, by severity |

## License

BSD 2-Clause. See [LICENSE](LICENSE).
