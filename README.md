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
--enable-asan           AddressSanitizer                       (default: no)
--enable-lsan           LeakSanitizer                          (default: no)
--enable-msan           MemorySanitizer, clang on Linux        (default: no)
--enable-ubsan          UndefinedBehaviorSanitizer             (default: no)
--enable-coverage       gcov or llvm-cov instrumentation       (default: no)
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
```

Each sanitizer suite configures a disposable build tree beside the current
one, so it never leaves instrumented objects in the tree you work in. A
sanitizer the platform does not support reports that and succeeds rather than
failing the build.

## Embedding

`librelic` installs a single public header, `<relic.h>`, and a pkg-config
module:

```sh
cc $(pkg-config --cflags librelic) embed.c $(pkg-config --libs librelic)
```

Only symbols matching `relic_` are exported.

## Documentation

| File | Contents |
| --- | --- |
| [specs/architecture.md](specs/architecture.md) | the target design of the system |
| [specs/style.md](specs/style.md) | code style, binding on all relic-owned code |
| [relic-plan.md](relic-plan.md) | the staged implementation plan and decision record |
| [STATUS.md](STATUS.md) | what exists today |
| [TODO.md](TODO.md) | open work, by severity |

## License

BSD 2-Clause. See [LICENSE](LICENSE).
