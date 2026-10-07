# relic relation export design

**Status.** Design document for writing a whole relation to a file: how an
export is named on the command line, which predicate and arity it names,
what it writes in each format, and where it falls among the tool's phases.
The language does not change, and no statement of a program names a file.
Forward-looking, like every document under `specs/`; `TODO.md` records what
is not yet implemented.

**Companions.** `specs/design-import.md` section 3, the operand an export
mirrors and the reason program text names no file; `specs/design-cli.md`
sections 2, 4, 5, 6 and 7, the phases, the formats an export writes, the
order `-s` gives, the output file it is written like and the write failures
it reports as; `specs/design-api.md`, the entry point it adds;
architecture sections 40, 50 and 69.

---

## 1. What this document settles

* how an export is named: an `-O` operand, either a path alone that names
  its format and predicate or a head that names them, with the arity when
  the predicate's name has more than one;
* which predicate an export reads, and that its contents are the answers to
  a query of all of its columns;
* the four formats an export writes, `csv`, `tsv`, `json` and `dl`, and
  which of them a later run reads back without losing a value's type;
* when exports are resolved and when they are written, among the phases of
  an invocation, and how they combine with the other options;
* the one entry point the library adds, which says at which arities a name
  is a predicate.

---

## 2. Why

A query already writes a relation: `relic -F tsv -q 'path(X, Y)?' tc.dl`
writes every tuple of `path/2` to standard output. Three things make that
the wrong tool for a program's results:

* **One stream holds one table.** Writing `path` and `scc` to their own
  files takes two runs, which evaluate the program twice, or one run whose
  output a script splits at the blank line between queries.
* **The query is a spelling of the arity.** `path(X, Y)?` restates what the
  program already says, and a predicate of eight columns is eight variable
  names to keep in step with it.
* **No format reads back as it was.** csv and tsv write a symbol and a
  string with the same bytes alike (`specs/design-cli.md` section 4.2), so a
  relation that mixes them cannot be imported again, and text is meant for
  a person.

An export names a predicate and a file. It is Souffle's `.output`, given on
the command line rather than in the program, and the counterpart of `-I`:
what a run exports, a later run imports, or loads, with nothing to restate
but what the format leaves out.

---

## 3. Naming an export

### 3.1 On the command line

```text
-O path
-O format:predicate[/arity]=path
```

```sh
relic -O path.tsv tc.dl
relic -O path.tsv -O scc.csv -a tc.dl
relic -O dl:path=closure.facts tc.dl
relic -O tsv:edge/3=weighted.tsv graph.dl
```

The operand's grammar:

```text
export     := path | head "=" path
head       := format ":" predicate [ "/" arity ]
format     := "csv" | "tsv" | "json" | "dl"
arity      := [0-9]+, at most 32
predicate  := [a-z][A-Za-z0-9_]*, other than "not"
path       := one or more bytes
```

`-O` is repeatable, one export per operand. It is spelled as `-I` is
(`specs/design-import.md` section 3.1): an operand that holds a `=` has a
head, and its path is everything after the first `=`; one with no `=` is a
path alone. A malformed operand is a usage error, status 2, reported in the
words of an import's:

```text
relic: error: invalid export: unknown format 'tvs' in tvs:path=out.tsv
relic: error: invalid export: no .csv, .tsv, .json or .dl extension, and no '=' before a path in closure
```

**A path alone names its format and its predicate**, as an import's does.
The extension, `.csv`, `.tsv`, `.json` or `.dl`, is the format, and the rest
of the file name is the predicate: `-O out/path.tsv` exports `path` as tsv.
A path whose file name has none of the extensions, or whose rest is not a
predicate name, is a usage error that asks for a head, and so is a path that
holds a `=`.

**The arity is named only when the name leaves it open.** A predicate is a
name and an arity together, and `edge/2` and `edge/3` are unrelated
predicates. An export whose head gives no arity, and every path alone,
exports the one predicate the program has of that name, and is an error
when it has several (section 3.2). The head's `/arity` chooses among them.
An arity has no column types to go with it, since nothing an export writes
depends on a column's type: each value is written as its own type's
spelling.

**Standard output is not a path.** An export to `-`, with a head or
without, is a usage error: a relation written to standard output is a
query's answer, which `-q` and `-F` already write, and a second writer of
standard output would interleave with the query rows. A file named `-` is
spelled `./-`.

**An export writes no path the command line spells otherwise.** Its path
may not be another export's, the `-o` file, an imported table or a program
operand; each is a usage error. An export truncates its file, so it would
destroy what another writer wrote, or replace an input with one of its own
relations -- a program named after its main predicate, `path.dl`, with the
facts of `path`, when the export is `-O path.dl`. The paths are compared as
written. That catches the mistake a command line makes by naming one file
twice, and is not a guarantee: `out.tsv` and `./out.tsv`, a file that
standard input or output was redirected to, and a path through `/dev/fd`
are other spellings, and an export to one of them truncates the file as
`-o` would. Following every spelling of a file to its identity would take
the tool opening every file the run reads itself, and each platform's
`/dev/fd` names files its own way.

### 3.2 Which predicate

An export reads a predicate the program knows: one that a statement, a fact,
an import or a query has named. A predicate the program knows but nothing
defines -- read by a rule and derived by none, or imported from an empty
table, which defines nothing (`specs/design-import.md` section 6.2) -- is
an empty relation. Its export writes an empty file and the
undefined-predicate warning, as a query of it does. A name the program
does not know at all is an error, since it is most often a misspelling:

```text
relic: error: no predicate 'pathh' to export in pathh.tsv
relic: error: 'edge' is a predicate of arities 2 and 3; name one in a head, as edge/2, in edge.tsv
relic: error: no predicate 'edge/4' to export in tsv:edge/4=edge.tsv
```

These depend on the program and not on the command line, so they are
reported in phase 4, beside a query's mistakes, with status 1, and nothing
is evaluated. The arities come from the library, which alone holds the
predicate table (section 7).

---

## 4. What is written

**An export's contents are the answers to a query of all of its
columns.** `-O path.tsv` writes exactly the rows `-F tsv -q 'path(V1, V2)?'`
writes: every tuple of the relation, once, with each value spelled as the
format of `specs/design-cli.md` section 4 spells it. The export prepares
that query, named `<export N>` by its position among the `-O` options, and
runs it, so a run that materialized the program with `-a` reads the
materialized relation, and one that did not evaluates what the predicate
depends on through the Magic rewrite. Each export is a query of its own,
and a run that exports several predicates of one program does well to
materialize it first with `-a`.

**No header is written.** relic's columns are positional and have no
names (architecture section 10.1), and a header of types alone would be a
row of symbols that an import with column types would read as data. What a
column holds is the program's to say, and what a later import reads it as
is that import's `-T`.

**Order.** Rows are written in the query's order, which is deterministic
for a given input, or sorted with `-s` as a query's are
(`specs/design-cli.md` section 5).

### 4.1 csv and tsv

Each row is a line of the format the tool's `-F csv` and `-F tsv` write,
and an import reads back (`specs/design-import.md` section 5). The formats
are untyped: a symbol and a string with the same bytes are written alike.
A relation whose every column holds one type is read back with a `-T` that
names those types:

```sh
relic -O path.tsv tc.dl
relic -T path:symbol,symbol -I path.tsv report.dl
```

A column that holds more than one type is written as the format writes
each value, and an import of the file cannot recover which was which; `dl`
can.

### 4.2 json

The document of `specs/design-cli.md` section 4.4: one array of rows, each
an array of values, in which a string is the quoted literal and a symbol
its bytes, so that every value keeps its type. It is for a consumer other
than relic, which imports no json.

### 4.3 dl

Each row is a fact of the exported predicate, one per line, written as a
program writes it: the predicate's name, its values in parentheses
separated by `, `, and a full stop. A symbol is its bytes, a string the
quoted literal of `specs/design-cli.md` section 4.1, and an integer its
decimal. A predicate of no columns is its name and the full stop.

```text
path(a, b).
path(a, "Ann Lee").
path(b, -7).
done.
```

**A dl export is a program, and loads back as one.** Every value is spelled
by the language's own rules, so a later run that names the file as a
program operand holds exactly the relation that was exported, mixed
columns included, with no `-T`:

```sh
relic -O path.dl tc.dl
relic path.dl report.dl
```

The literal escapes every control byte and every byte that does not begin
valid UTF-8, as text output does, so a dl export is safe to print and is
valid UTF-8 whatever its strings hold. A dl file is loaded as a program and
not imported: `-I` reads tables, and a file of facts is already the
language.

---

## 5. Where exports fall among the phases

An export is resolved with the queries and written after them. Among the
phases of `specs/design-cli.md` section 2:

1. **Parse.** A malformed `-O` operand, or an export to a path the
   command line names otherwise (section 3.1), is a usage error.
2. **Load**, and 3. **check**, as `specs/design-cli.md` section 2 says.
4. **Prepare.** Each export's predicate is resolved (section 3.2) and its
   query prepared, after the queries, and every mistake in either is
   reported before anything runs.
5. With `-c`, stop: the exports are valid, and nothing is written.
6. With `-a`, materialize.
7. **Open** the output, and 8. **run** the queries, as
   `specs/design-cli.md` section 2 says.
9. **Export** each relation in command-line order: write the relation to
   its file, opened with the first bytes, and close it. The first export
   that fails ends the run.
10. With `-i`, the interactive session, and 11. **close** the output.

**Exports follow the queries** so that the answers a person is waiting for
are written first, and **precede the session** so that what they hold does
not depend on what the session did. A session adds nothing to the program
(`specs/design-cli.md` section 10.1), but it may hit a limit, and an export
written before it is complete whatever the session meets.

**Each file is opened when its first bytes reach it**, created or truncated
as `-o`'s is (`specs/design-cli.md` section 6), or once its query has
succeeded without writing any, which creates an empty file. The library
delivers no row until everything in the evaluation that can fail has
succeeded, so an export whose evaluation fails -- at a limit, for memory --
leaves its file as it was, as does a run that fails before the export. What
can fail once a row has been written is the writing itself, reported naming
the file, and an interrupt, which outside an interactive session ends the
process, as it does a run's queries (`specs/design-cli.md` section 10.4).
Either leaves the file holding the rows written before it. Writing to a
temporary file and renaming it would spare a reader that partial file, at
the price of replacing a symbolic link rather than writing through it and of
the file mode an existing file had; the tool writes every file it writes as
`-o` does, and the exit status is what says whether a file is whole.

**A write failure** is reported once, naming the file, as an output's is
(`specs/design-cli.md` section 7), and ends the run. Closing a file whose
writes succeeded can still fail, and that is a failure of the export too.

### 5.1 Combinations

| Option | Effect on an export |
| --- | --- |
| `-a` | the export reads the materialized relation |
| `-c` | the export is resolved and prepared, and nothing is written |
| `-e` | the plan of the export's query is written, as a query's is |
| `-F` | none: an export's format is its operand's |
| `-n` | none: `-n` discards query rows, and an export is not a query's output |
| `-o` | none, but the two cannot name one path |
| `-q` | none: the queries run, then the exports |
| `-s` | the export's rows are sorted |
| `-v` | a line after each export, `export N: M rows`, as after a query |

Exports do not stop the program's own queries from running when there is no
`-q`, and a run whose program has no queries and no `-q` evaluates what its
exports need and nothing else.

---

## 6. The tool

The operand is parsed by the code that parses an import's, which already
reads a head, a path alone, an extension and a predicate name; the export's
formats and its `/arity` are what differ. The rows are written by the
output of `specs/design-cli.md` section 4.7, one per export attached to the
export's file, so the formats, the buffer, the sort and the write failures
have one implementation. `dl` is a format of that output which only an
export selects: a query's rows are not facts of any predicate, and `-F`
does not offer it.

---

## 7. Through the library

```c
int	relic_predicate_arities(relic_t *, const char *name,
	    uint64_t *arities);
```

`relic_predicate_arities()` sets `*arities` to the set of arities at which
the context knows a predicate named `name`: bit `n` is set when a
statement, a fact, an import or a query has named `name/n`. A name the
context does not know sets no bit. It begins with a check, as every
operation that uses the program does, so that a program's rules have been
compiled and their predicates named, and fails as that check fails. It is
refused, with a diagnostic, when `name` or `arities` is NULL or the context
is busy. It returns 0, or -1 on failure with `*arities` untouched.

A set of arities is a word because an arity is at most 32, so each fits a
bit of 64, and a caller asks one question per name rather than one per
arity. Predicates the Magic rewrite generates, and the group predicates of
aggregates, are the library's and are never reported.

An export needs nothing else of the library: the query of all columns is
prepared and run through the query interface, whose text the tool writes
from a name it has already validated and an arity at most 32.

---

## 8. Determinism

An export writes the same bytes for the same program, data and options,
under any `-j`, as a query does: its rows are a query's, in the query's
deterministic order or sorted. Which of several exports runs first is the
command line's order.

---

## 9. Security

An export writes only files the command line names. A program cannot name
one, for the reasons `specs/design-import.md` section 3.3 gives for imports:
a program is data that may come from anyone, and the person who runs the
tool decides what it touches. A path is opened as given, following a
symbolic link, with the mode `fopen(3)` gives a new file, as `-o` is.

csv and tsv write a string's bytes as they are, control bytes included, as
query output in those formats does; json and dl escape every byte that is
not printable, so their files are safe to print.

---

## 10. The tests

* **Operands.** Every malformed operand: an unknown format, an empty
  predicate, an arity that is not a number or exceeds 32, a path alone with
  no extension, an unknown extension, a stem that is not a predicate name,
  an empty path, `-` with a head and without; and a path another export,
  `-o`, an import or a program operand spells alike.
* **Resolution.** An unknown name, an unknown arity, a name with two
  arities by path alone and by a head without one, and the head with an
  arity that chooses one; an export of a predicate read but never derived,
  and of one imported from an empty table, writes an empty file with the
  warning.
* **Formats.** One relation holding a symbol, a string with the same bytes,
  a string with a tab, a newline, a quote, a backslash and a control byte,
  a negative integer and the extremes of `int64_t`, exported in each format
  and compared byte for byte; a predicate of no columns, holding its fact
  and not; the dl export loaded back as a program and queried, equal to the
  original relation; a csv and a tsv export imported back with `-T`.
* **Phases.** Exports after the queries and in command-line order; `-c`
  writes nothing and reports a bad export; a failing query leaves every
  export's file as it was; `-s`, `-v`, `-e`, `-n` and `-a` as section 5.1
  says; an existing file truncated.
* **Failures.** A file that cannot be opened and a write the system
  refuses, each reported once naming the file, and an evaluation that
  meets a limit during an export, which leaves the file as it was; each
  with status 1, the exports after it not run.
* **Library.** `relic_predicate_arities()` over a program naming one name
  at several arities, through a rule, a fact, an import and a query; a name
  only the Magic rewrite generates; NULL arguments and a busy context.

---

## 11. Amendments this design makes

* Architecture:
  * section 40: `-O export` in the usage and the option list;
  * section 41: an export example;
  * section 50: `relic_predicate_arities()`;
  * section 64: `cmd/relic/export.c`, and `cmd/relic/operand.c` for the
    operands of `-I`, `-T` and `-O`;
  * section 69: relation export points here and leaves the list;
  * section 76: logs `-O`, the entry point and D-39.
* `specs/decisions.md`: D-39, an export as the query of all of a
  predicate's columns, named as an import is.
* `specs/design-cli.md`: section 2, the export phase; section 2.3, the
  usage errors.
* `specs/design-api.md`: the entry point.
* `specs/design-import.md` section 15: the writing of tables it leaves out
  is this design.
* `include/relic.h`, `man/relic.1` and `man/relic.3`.
* `TODO.md` follows the implementation.

---

## 12. What is deliberately absent

* **An export directive in program text** (section 9).
* **A header**, typed or not (section 4).
* **Standard output** (section 3.1).
* **Replacing a file atomically** (section 5).
* **Exporting every predicate.** A program's relations include those it
  only uses on the way to its answers, and naming the ones wanted is one
  operand each.
* **Choosing or reordering columns.** A query does that: `-F tsv -q
  'path(Y, X)?'`.
* **Text as an export format.** It is written for a person, and dl is the
  spelling that reads back.
