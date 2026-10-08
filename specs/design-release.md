# Releases

What makes a relic release and what must hold before one is made: the
interface documentation and the test that keeps its examples true, the
tarball and what it carries, the packaging skeletons, the continuous
integration matrix and its policy, the release gate, and the steps that
produce a release. It is engineering around the system: nothing here
changes what relic computes.

Related: [build](architecture.md#build),
[portability](architecture.md#portability),
[C library and ABI](architecture.md#c-library-and-abi),
[versioning](design-api.md#versioning), [testing](design-testing.md).

## Model

A release is a tagged commit, the tarball `make distcheck` produces from
it, its `NEWS` entry, and the packaging files that describe that tarball.
A release's users build from its tarball, not from the repository, so
everything that decides whether relic works for them -- the generated
`configure`, the tests, the documentation and its examples -- is checked as
the tarball carries it.

## Interface documentation

The manual pages are the interface documentation: `relic(1)` for the tool
and the language, `relic(3)` for the library. They are mdoc, and
`tests/man.sh` judges their markup with `mandoc -Tlint`, holds the names
`relic(3)` lists to the header's exports, and holds the options `relic(1)`
describes to the usage message. The architecture and design documents under
`specs/` are design documentation; neither kind substitutes for the other.

`README.md` takes a reader from a tarball to a first query and a first
embedding: building, a program and its answer, and compiling against the
installed library. `EXAMPLES.md` is a tour of the language through programs
a reader can run. Both point to the manual pages for everything else.

`NEWS` records each release's changes for its users, newest first: a heading
naming the version and date, and a list of what changed that a user or an
embedder would notice. It is written at the release, not between releases.

## Documented examples are tests

An example that was never run is the text most likely to be wrong, and it
goes wrong silently: nothing fails when a later change makes it false.
`tests/docs.sh` therefore runs every example in `man/relic.1`,
`man/relic.3`, `README.md` and `EXAMPLES.md`, and fails when one is not run.

### What is an example

A **command line** is a line of an example block that begins `$ `: in mdoc a
`.Dl` line or a line inside `.Bd -literal`, and in Markdown a line inside a
fenced block. The rest of the line is the command as a shell reads it,
quotes and redirections included. Every command line runs `relic`, and every
line of a document that holds `$ relic ` must be a command line, so the
check fails closed: an example written in markup it does not read is an
error, not an example left unrun.

A **session** is a command line that starts interactive mode, followed in its
block by lines beginning `relic> `, each one line of input, and the lines
between and after them, the output. With input that is not a terminal relic
prints no prompt, so a session's input lines are its standard input and its
other lines its standard output.

The **C example** of `relic(3)` is the one literal block of its EXAMPLES
section, and it is a complete program: an example a reader cannot compile
teaches less than one they can, and only a complete program can be checked
as it stands. It is `examples/query.c` verbatim, copyright notice aside, so
the program `make check` builds and the program the page shows are one.

A fenced block of a Markdown document whose info string names a file after
its language, as ```` ```text graph.dl ```` does, **shows** that fixture of
`tests/examples/` and must hold exactly its bytes: a program a reader is
shown and then asked to run is the program the example runs.

Build and installation commands -- `./configure`, `make`, `make install`, the
`cc` line that compiles against the installed library -- are not command
lines in this sense. Continuous integration runs them as written: the
tarball jobs configure, build and test a tarball where no autotools exist,
and `make distcheck` installs into a staging directory, where
`make installcheck` builds `examples/embed.c` through `pkg-config` against
what was installed and never against the build tree.

### Expectations

`tests/examples/expected` holds what each example must do, in the form the
documentation shows it:

```text
$ relic -c broken.dl
2> broken.dl:1:1: error: ...
# exit 1
```

An entry is a command line, the lines it must write to standard output, the
lines it must write to standard error each marked `2> `, and an optional
`# exit N` line, zero when absent. A session's entry is its whole block,
input lines included. When the documentation shows a transcript it must be
the entry with the markers removed, so what a reader sees is what is
checked; a command line shown without one is still run against its entry.
The C example's entry is its output, under the command line `$ ./query`.

The files the examples name are fixtures in `tests/examples/`, written so
that every example does what its sentence says it does: a program the page
checks is one that checks, a query the page answers has rows.

### The check

`tests/docs.sh` extracts every command line, session and the C example from
the documents, unescaping mdoc's escapes, and reads the entries of
`tests/examples/expected`. The two sets must be equal in both directions,
as `tests/langfiles.sh` holds the language tests to their directory: an
example with no entry fails, so a new example cannot go unrun, and an entry
with no example fails, so a removed one cannot leave a test of nothing
behind.

Each command runs in a copy of `tests/examples/` through `sh -c`, with
`relic` defined as a function calling the built tool, so quoting and
redirection mean what they mean to a reader's shell. The documents and the
fixtures are therefore trusted as any test is: what they say runs with the
tester's privileges. Its output and status are compared with the entry's.
The C example is built by `make check` with only the
public header on its include path, as an embedder builds it, compared with
the page's block, and run.

`make check` runs the check, and `make distcheck` runs it from the tarball,
which therefore carries the fixtures.

## The tarball

`make dist` produces `relic-VERSION.tar.gz`. It carries the sources, the
generated `configure` and makefiles, the tests and their data, the fuzz
corpora and dictionaries, the manual pages, `README.md`, `EXAMPLES.md`,
`NEWS`, `LICENSE`, the relic-only benchmark instrument, and the project's
documents -- `specs/`, `docs/` and `TODO.md` -- so whoever audits the code
has its design and its open questions beside it. `specs/` and `docs/` are
distributed whole, by directory, so a new document cannot be left out.

It builds with a C11 compiler, `make` and a POSIX shell, and nothing else:
the autotools are maintainer tools, `libedit` is optional, and `mandoc`,
`nm` and `pkg-config` are wanted by some tests and by nothing that is built.
A test that lacks one skips, and a strict run turns that skip into a
failure (see [testing](design-testing.md)).

## Packaging skeletons

`contrib/` holds a starting point for each packaging system relic targets,
for a packager to adapt rather than for the build to use:

```text
contrib/freebsd/devel/relic/     Makefile, distinfo, pkg-descr, pkg-plist
contrib/openbsd/devel/relic/     Makefile, distinfo, pkg/DESCR, pkg/PLIST
contrib/pkgsrc/devel/relic/      Makefile, distinfo, DESCR, PLIST
contrib/homebrew/relic.rb        a formula
```

`contrib/` lives in the repository and not in the tarball, since it
describes the tarball: a file inside a tarball cannot hold that tarball's
checksum. Each skeleton fetches the release tarball from
`https://github.com/stass/relic/releases/download/vVERSION/relic-VERSION.tar.gz`,
configures it with the system's conventions, runs `make check` where the
system has a test target, and installs the paths the
[build](architecture.md#build) contract names; the packing lists name those
paths exactly. The checksums in the `distinfo` files and the formula are
those of the published tarball, so they are written once it exists, by the
commit after the release's tag. A skeleton describing no published tarball
carries an all-zero checksum, so an install fails on the mismatch rather
than trusting an unchecked download.

## Continuous integration

### Policy

Every job passes on the commit a release is made from. Between releases,
the jobs that are cheap run on every push to the main branch and every pull
request; the slower configurations run on a schedule; and a platform's job
may run by hand while its runners are costly.

A job that cannot run what it is for fails rather than skips. Every job sets
`RELIC_STRICT_CHECK=yes`, which turns a skipped test, or a sanitizer target
the compiler cannot build, into a failure; `RELIC_ALLOWED_SKIPS` names the
few skips a job expects, such as the export-table audit in a build with no
shared library. A job that passes while checking nothing is worse than one
that fails.

The ASCII-only rule for sources and `mandoc -Tlint` over both manual pages
are tests of the suite (`tests/ascii.sh`, `tests/man.sh`), so every job that
runs `make check` runs them, and strict mode makes a job without `mandoc`
fail rather than skip the lint. They need no jobs of their own.

### Matrix

| Job | Purpose | Runs |
| --- | --- | --- |
| Linux gcc, glibc | primary, `--enable-werror` | every push |
| Linux clang | the second front end's warnings, `--enable-werror` | every push |
| Linux clang, ASan + UBSan | `make check-asan`, `make check-ubsan`, leaks included | every push |
| distcheck | out-of-tree build, `DESTDIR` install, installcheck, all from the tarball | every push |
| tarball, glibc and musl | `make dist` on Linux, then configure, build and test it in Debian and Alpine containers with no autotools installed | every push |
| Linux clang, MSan | uninitialized reads ASan cannot see | weekly |
| Linux clang, TSan | `make check-tsan`, the pool's threads under ThreadSanitizer | weekly |
| Linux gcc `-m32` | `size_t` and pointer-width assumptions | weekly |
| Linux gcc, no threads | `--disable-threads`: the pool is the calling thread alone | weekly |
| distcheck variants | `--disable-shared` and `--disable-static` | weekly |
| macOS clang | the development platform: `--enable-werror`, ASan (no leak detection there) and UBSan | by hand |

The workflows are `.github/workflows/ci.yml` (every push and pull request),
`extended.yml` (weekly and on demand) and `macos.yml` (on demand).

The tarball jobs are the ones most likely to catch a real user-visible
break, because they are the only ones that build what users download, on a
system that has never seen the repository. The musl job exercises the
`compat/` replacements a glibc build never compiles.

## The release gate

A release is made only when all of the following hold on the release
commit:

1. Every job of the matrix passes. FreeBSD, OpenBSD and NetBSD, the primary
   target platforms, which no workflow reaches, are built and tested from
   the tarball by hand, with the result recorded in the release's `NEWS`
   entry. The tarball must build, install and pass its own suite with no
   autotools present on FreeBSD, OpenBSD, NetBSD, macOS, and Linux with
   both glibc and musl.
2. The full suite runs clean under AddressSanitizer with leak detection and
   under UndefinedBehaviorSanitizer.
3. relic has no third-party runtime dependency beyond the optional
   `libedit` (see [dependency policy](architecture.md#dependency-policy)).
4. The interface version in `configure.ac` matches the interface changes
   since the last release, by the rules written beside it
   ([versioning](design-api.md#versioning)).

The functional promises -- that relic validates programs, rejects unsafe and
unstratified ones with a diagnostic naming the cycle, evaluates recursion
semi-naively, answers a bound query through the Magic rewrite and by
whole-program evaluation with identical results, gives identical results
with and without indexes and reordering, shows its plans with `-e`, and
streams rows through the library without materializing a result array -- are
not a checklist at release time. Each is held by the suites on every commit (see
[testing](design-testing.md)), so a green matrix is the evidence.

## The release procedure

1. `configure.ac`'s version becomes the release's, and its interface
   version is checked against the rules written beside it: a release
   carries whatever interface change was made since the last.
2. `NEWS` gains the release's entry, and `TODO.md` becomes the next
   release's agenda.
3. The gate above is met on the commit to be released.
4. The commit is tagged `vVERSION`, and `make distcheck` on the tag produces
   the tarball, which is published with its SHA-256 as the GitHub release of
   that tag.
5. The next commit writes that SHA-256 and the version into the `contrib/`
   checksums and the formula, so a packager fetching the published tarball
   gets the bytes `contrib/` describes.
