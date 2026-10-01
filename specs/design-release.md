# Release: documentation that runs, packaging, and the release procedure

What makes a relic release: the interface documentation, the test that keeps
it true, the tarball and what it carries, the packaging skeletons, and the
steps that produce a release. Forward-looking, like every document under
`specs/`; what is not yet done is in `TODO.md`.

## 1. The interface documentation

The manual pages are the interface documentation: `relic(1)` for the tool and
the language, `relic(3)` for the library. They are mdoc, and `mandoc -Tlint`
judges their markup in `tests/man.sh`, which also holds the names `relic(3)`
lists to the header's exports and the options `relic(1)` describes to the
usage message. The architecture and the design documents are design
documentation; neither kind substitutes for the other.

`README.md` takes a reader from a tarball to a first query and to a first
embedding: building, a program and its answer, and compiling against the
installed library. It points to the manual pages for everything else.

`NEWS` records each release's changes for its users, newest first: a heading
naming the version and date, and a list of what changed that a user or an
embedder would notice. It is written at the release and not between them.

## 2. Documented examples are tests

An example that was never run is the text most likely to be wrong, and it
goes wrong silently: nothing fails when a later change makes it false.
`tests/docs.sh` therefore runs every example the manual pages and the README
show, and fails when one is not run.

### 2.1 What is an example

A **command line** is a line of an example block that begins `$ `: in mdoc
a `.Dl` line or a line inside `.Bd -literal`, and in `README.md` a line
inside a fenced block. The rest of the line is the command as a shell reads
it, quotes and redirections included. Every command line runs `relic`, and
every line of a document that holds `$ relic ` must be a command line, so
the check fails closed: an example written in markup it does not read is an
error, not an example left unrun.

A **session** is a command line that starts interactive mode, followed in its
block by lines beginning `relic> `, each the input of one line, and the lines
between and after them, the output. With input that is not a terminal relic
prints no prompt, so the session's input lines are its standard input and its
other lines are its standard output.

The **C example** of `relic(3)` is the one literal block of its EXAMPLES
section, and it is a complete program: an example a reader cannot compile
teaches less than one they can, and only a complete program can be checked
as it stands. It is `examples/query.c`, its copyright notice aside, so that
the program `make check` builds and the program the page shows are one.

A fenced block of `README.md` whose info string names a file after its
language, as ```` ```text graph.dl ```` does, **shows** that fixture of
`tests/examples/`, and must hold exactly its bytes: a program a reader is shown
and then asked to run is the program the example runs.

Build and installation commands -- `./configure`, `make`, `make install`, the
`cc` line that compiles against the installed library -- are not command
lines in this sense. The continuous integration jobs run them as written:
the tarball job configures, builds and tests a tarball where no autotools
exist, `make distcheck` installs into a staging directory, and its
installcheck builds `examples/embed.c` through `pkg-config` against what was
installed.

### 2.2 Expectations

`tests/examples/expected` holds what each example must do, in the form the
documentation shows it:

```text
$ relic -c broken.dl
2> broken.dl:1:1: error: ...
# exit 1
```

An entry is a command line, the lines it must write to standard output, the
lines it must write to standard error each marked `2> `, and an optional
`# exit N` line, zero when absent. A session's entry is its whole block, its
input lines included. When the documentation shows a transcript, it must be
the entry with the markers removed, so what a reader sees is what is
checked; a command line shown without one is still run against its entry.
The C example's entry is its output, under the command line `$ ./query`.

The files the examples name -- `graph.dl`, `project.dl`, `edge.tsv`,
`people.csv` and the rest -- are fixtures in `tests/examples/`, written so that
every example does what its sentence says it does: a program the page checks
is one that checks, a query the page answers has rows.

### 2.3 The check

`tests/docs.sh` extracts every command line, session and the C example from
`man/relic.1`, `man/relic.3` and `README.md`, unescaping mdoc's `\e`, `\-`,
`\&`, `\(aq` and `\(dq`, and reads the entries of
`tests/examples/expected`. The two sets must be equal in both directions,
as `tests/langfiles.sh` holds the language tests to their directory: an
example with no entry fails, so a new example cannot go unrun, and an entry
with no example fails, so a removed one cannot leave a test of nothing
behind.

Each command runs in a copy of `tests/examples/` through `sh -c`, with `relic`
defined as a function calling the built tool, so that quoting and
redirection mean what they mean to a reader's shell. Its output and status
are compared with the entry's. The C example is built by `make check` with
the flags every test program is built with, compared with the page's block,
and run.

The check is a test like any other: `make check` runs it, and `make
distcheck` runs it from the tarball, which therefore carries the fixtures.

## 3. The tarball

`make dist` produces `relic-VERSION.tar.gz`. It carries the sources, the
generated `configure` and makefiles, the tests and their data, the manual
pages, `README.md`, `NEWS` and `LICENSE`, and the project's documents --
`specs/`, `docs/` and `TODO.md` -- so that whoever audits the code has its
design and its open questions beside it. `specs/` and `docs/`
are distributed whole, so a new document cannot be left out.

It builds with a C11 compiler, `make` and a POSIX shell, and nothing else:
the autotools are maintainer tools, `libedit` is optional, and `mandoc`,
`nm` and `pkg-config` are wanted by some tests and by nothing that is built.

## 4. Packaging skeletons

`contrib/` holds a starting point for each packaging system relic targets,
for a packager to adapt rather than for the build to use. It lives in the
repository and not in the tarball, since it describes the tarball: a file
inside one cannot hold that tarball's checksum.

```text
contrib/freebsd/devel/relic/     Makefile, distinfo, pkg-descr, pkg-plist
contrib/openbsd/devel/relic/     Makefile, distinfo, pkg/DESCR, pkg/PLIST
contrib/pkgsrc/devel/relic/      Makefile, distinfo, DESCR, PLIST
contrib/homebrew/relic.rb        a formula
```

Each fetches the release tarball from
`https://github.com/stass/relic/releases/download/vVERSION/relic-VERSION.tar.gz`,
configures it with the system's conventions, runs `make check` where the
system has a test target, and installs the paths of architecture section
65.5. The packing lists name those paths exactly. The checksums in the
`distinfo` files and the formula are those of the published tarball, and so
are written once it exists.

## 5. The release procedure

1. `configure.ac`'s version becomes the release's, and its interface
   version is checked against the rules written beside it: a release
   carries whatever interface change was made since the last.
2. `NEWS` gains the release's entry, and `TODO.md` becomes the next
   release's agenda.
3. Every continuous integration job passes on the commit to be released,
   and the platforms CI does not reach are built and tested from the
   tarball by hand, with the result recorded in the release's `NEWS`
   entry.
4. The commit is tagged `vVERSION`, and `make distcheck` on the tag
   produces the tarball, which is published with its SHA-256 as the GitHub
   release of that tag.
5. The next commit writes that SHA-256 and the version into the `contrib/`
   checksums and the formula, so that a packager fetching the published
   tarball gets the bytes `contrib/` describes.
