# relic command-line tool design

**Status.** Design document for the `relic` tool: what an invocation does and
in what order, which queries it runs, what it exits with, and the exact form of
every output format. Forward-looking, like every document under `specs/`;
`STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 40 to 44 and 61;
`specs/design-eval.md` for how a query is answered; `relic-plan.md` decisions
D-5 and D-15.

---

## 1. What this document settles

* the phases of an invocation, and what stops at each;
* which queries run: the ones written in the program, or the ones on the
  command line;
* the three exit statuses and what maps to each;
* the text, csv, tsv and json formats, byte for byte;
* how the results of several queries are told apart;
* what `-s` sorts by and what `-o` does with a file;
* what `-n` suppresses and what each level of `-v` adds;
* the interactive mode of `-i`: what it reads, its commands, and how a
  session ends.

The option set itself is architecture section 40's. The tool is a client of
`librelic` and reaches the engine only through `<relic.h>`; anything it needs
that the header does not offer is a finding against the header.

---

## 2. An invocation

An invocation runs in phases, and each phase runs only if the one before it
succeeded:

1. **Parse the command line.** A malformed one is a usage error.
2. **Load** every file operand, or standard input when there are none. Every
   file is attempted even after one fails, so that one run reports the mistakes
   in all of them.
3. **Check** the program.
4. **Prepare** every query that will run, reporting what is wrong with each of
   them before any runs. A mistake in the third `-q` is reported even when the
   first two are fine.
5. With `-c`, stop here: the program and its queries are valid.
6. With `-a`, **materialize** the whole program.
7. **Open** the output: the `-o` file, or standard output.
8. **Run** each query in turn and write its results. The first query that fails
   ends the run, because a failure in evaluation -- a limit, most often --
   would recur in every query after it.
9. With `-i`, run the **interactive session** of section 10.
10. **Close** the output, and report a write that failed.

Validation precedes evaluation throughout: a run either reports mistakes
without evaluating anything, or evaluates a program and a set of queries that
are all known to be valid.

### 2.1 Input

File operands are read in the order given and form one program. With no
operands, the program is read from standard input, under the name `<stdin>`,
provided standard input is not a terminal. With no operands and a terminal on
standard input the tool would wait, silently, for a program nobody is typing,
so that is a usage error instead; interactive use has an option of its own.

Under `-i`, standard input belongs to the session and is never read as
program text. The program is the file operands, and with none it is empty.

### 2.2 Which queries run

* With one or more `-q`, exactly those queries run, in command-line order. The
  program's own queries are still checked in phase 3, since they are part of
  the program, but they do not run.
* Without `-q`, the queries written in the program run, in load order.

A program with no queries and no `-q` is checked, and nothing is evaluated.
`-a` materializes it anyway, which is how a caller asks for evaluation
without a query -- to see whether a limit holds, say.

`-q` text is a query as the grammar writes it, ending in `?`. Diagnostics
about it point at `<query N>`, where N is its position among the `-q`
options, counting from one, so that a message about the third says so.

Every query an invocation is given to run has such a number, counting from
one: the `-q` options, or the program's queries when there are none, and then
each query line of the interactive session, and each `.plan`, in the order
read. For `-q` the two countings agree. A session's query line, and a
`.plan` with its query, takes its number whether or not the query can be
prepared, so that no two queries' diagnostics share a name; a line that is
refused before any query is read from it, such as an unknown command, takes
none. The number is what `-v` reports a query by and what a query typed in
the session is named in diagnostics. It is the tool's numbering and not the
library's: the answer predicate a plan names, `$q` and a serial, counts every
query the context has prepared, the program's own among them, from zero.

### 2.3 Combinations

`-V` stands alone. `-c` contradicts both `-a` and `-i`, since it forbids
evaluation and they demand it. Each conflict is a usage error, as is an
unknown option, an unknown `-F` format, or a missing operand to an option that
takes one. Options that do not apply in a mode -- `-e`, `-F`, `-o`, `-s` and
`-v` under `-c`, and `-F`, `-o` and `-s` under `-n` -- are accepted and have no
effect.

### 2.4 Plans

`-e` writes the plan of everything evaluated to standard error, each
component's as it is planned and each query's before its rows, in the text of
`specs/design-planner.md` section 8. Standard error rather than the output,
because a plan is about the run and not one of its results: standard output
or the `-o` file stays exactly the rows, parseable in any format.

---

## 3. Exit status

| Status | Meaning |
| --- | --- |
| 0 | every phase succeeded |
| 1 | the program or a query was rejected, evaluation failed, or input or output failed |
| 2 | the command line was malformed |

A query with no answers is a success. An exceeded limit is status 1, and the
query that exceeded it writes no rows, per architecture section 63.

---

## 4. Output formats

A query's result is a set of rows of equal width. Every format writes one row
per answer and writes each value according to its type, as below. Output is
byte-transparent in the sense of D-15: csv and tsv write a string's bytes as
they are, and text and json write every byte either as itself or as an escape
that spells it, so nothing a string holds is lost.

### 4.1 text, the default

Columns are separated by one space and each row ends with a newline. Each value
is written as it would be written in a program:

| Type | Written as |
| --- | --- |
| integer | decimal, with a leading `-` when negative |
| symbol | its bytes |
| string | a quoted literal, escaped as below |

A string literal escapes a backslash as `\\`, a double quote as `\"`, newline,
tab, carriage return and NUL as `\n`, `\t`, `\r` and `\0`. Every other byte that
is not part of a printable character is written as `\xHH`: the C0 controls and
DEL; a byte that does not begin a valid UTF-8 sequence; and both bytes of a C1
control, U+0080 to U+009F, which some terminals obey as the start of an escape
sequence just as they do ESC. Printable ASCII and every other UTF-8 character
are written unchanged, so text in any script reads as itself.

```text
u1 "Ann Lee" 7
```

Quoting strings is what keeps the format unambiguous: a symbol and a string
with the same bytes are different values and look different, and a space inside
a string cannot be mistaken for a column break. Escaping control characters is
what keeps it safe to print: a program cannot send a terminal escape sequence
to whoever reads its results. `\xHH` is an escape the language already has, so
the literal still spells exactly the bytes the string holds.

### 4.2 csv

RFC 4180 fields separated by commas, one record per line, each ending in a
newline rather than a carriage return and newline. Integers are written in
decimal; symbols and strings as their bytes, unescaped. A field is quoted when
it contains a comma, a double quote, a carriage return or a newline, or when it
is empty, and a double quote inside a quoted field is doubled.

```text
u1,Ann Lee,7
```

An empty string is quoted so that a record consisting of one empty string is
not an empty line, which some readers skip. csv is untyped, so a symbol and a
string with the same bytes are written identically.

csv and tsv are machine formats and carry control bytes unescaped, ESC
included. They are for a program to parse; printed to a terminal, the results
of an untrusted program in either can carry escape sequences, as `cat` of the
program itself could.

### 4.3 tsv

Fields separated by one tab, one record per line. Values are written as in csv,
except that instead of quoting, a backslash, tab, newline and carriage return
inside a value are written as `\\`, `\t`, `\n` and `\r`, so that neither
separator can appear inside a field.

```text
u1	Ann Lee	7
```

### 4.4 json

One document per query: an array of rows, each row an array of values, one row
per line.

```json
[
  ["u1","\"Ann Lee\"",7]
]
```

A query with no answers is `[]`.

| Type | Written as |
| --- | --- |
| integer | a JSON number |
| symbol | a JSON string holding its bytes |
| string | a JSON string holding the text format's quoted literal |

**A JSON string whose content begins with `"` is a string value; any other is a
symbol.** A symbol is an identifier and never begins with a quote, so the
convention is unambiguous, and a consumer recovers a string's bytes by
decoding the literal inside it with the escapes of section 4.1.

A JSON document must be valid UTF-8, and a string's bytes need not be. The
literal of section 4.1 is valid UTF-8 whatever the string holds, because a
byte that does not begin a valid sequence is written as `\xHH`, so the
document is valid and nothing is lost.

Integers are written exactly. A consumer that parses JSON numbers as double
precision floating point cannot represent every 64-bit integer, and one that
must should read numbers as integers.

### 4.5 Rows with no columns

A query with no variables, such as `edge(a, b)?`, asks whether something holds.
When it does, its result is one row of no values: an empty line in text, csv
and tsv, and `[]` as the one row of the json document. When it does not, there
are no rows.

```json
[
  []
]
```

### 4.6 Several queries

In text, csv and tsv, a blank line is written before the results of every
query but the first, whether or not either query had any rows. In json each
query's document ends with a newline and needs no other separator, so the
output of several queries is a sequence of documents, one per query, that a
streaming JSON reader consumes in turn.

---

## 5. Sorting

Result order is unspecified by the language, though deterministic for a given
input. `-s` sorts each query's rows before they are written, lexicographically
by column, using the total order of architecture section 61: integers before
symbols before strings, then numerically or bytewise within a type. The order
is the library's, obtained through `relic_value_compare()`, so the tool has no
second definition of it to drift.

Sorting holds a query's rows in memory, which unsorted output does not.

---

## 6. The output file

`-o file` writes results to `file` instead of standard output, creating it or
truncating it. It is opened in phase 7, after the program and every query have
been validated, so a run that fails to validate leaves an existing file alone.
A run that fails during evaluation leaves the file holding whatever the queries
before the failing one wrote.

Diagnostics never go to the output, per architecture section 43.

---

## 7. Write failures

A write can fail -- a full disk, a closed pipe -- and a tool whose output is
being consumed by a script must not report success when it was cut short. Every
write is checked, the stream is flushed and closed explicitly, and a failure at
any point is reported once, naming the output, and ends the run with status 1.

---

## 8. Suppressed output

`-n` runs every query as usual and writes none of its rows. The output is
never opened, so `-o` neither creates nor truncates its file, and `-F` and
`-s` have nothing to act on. What remains is everything written to standard
error: diagnostics, plans under `-e`, counts under `-v` and counters under
`-X stats`. `-n -v` is how to learn how many answers a query has without
printing them, and `-n -e` how to see what a query would run.

---

## 9. Verbosity

Diagnostics -- errors, warnings and the notes that accompany them -- are
written at every level. Each `-v` adds to them:

| Level | Adds |
| --- | --- |
| 1, `-v` | a line to standard error after each query that succeeded, `query N: M rows`, or `1 row` |
| 2, `-vv` | a note for the first ordering comparison in each rule that compared values of different types |

A third `-v` adds nothing more.

The count follows the query's rows, so in a session it appears under the
answer it describes. Like the counters of `-X stats` it is data about the run
rather than a complaint, and is written without the program name, so that its
text is the same however the tool was invoked.

An ordering comparison between values of different types is false, and the
binding is filtered out rather than reported as an error, because a column may
hold more than one type. Level 2 exists so that mixing of types nobody meant
remains discoverable. The note points at the comparison and names the two
types:

```text
rules.dl:4:22: note: ordering comparison between an integer and a symbol is false
```

It is reported by the library, which the tool asks for with the
`RELIC_F_NOTE_MIXED` switch of `specs/design-api.md`, once per rule of the
program for the whole run and once per query each time it is answered. The
rules a Magic rewrite generates from one rule, and the semi-naive variants of
it, count as that rule.

---

## 10. Interactive mode

`-i` enters an interactive session once the queries of phase 8 have run. The
session reads standard input a line at a time and answers each line before
reading the next, so it works the same from a terminal and from a pipe, and a
scripted session is an ordinary way to use it.

### 10.1 Lines

Each line is exactly one of:

| Line | What happens |
| --- | --- |
| empty, or only blanks | nothing |
| first non-blank byte `%` | nothing; it is a comment, as in a program |
| first non-blank byte `.` | the command it names, section 10.2 |
| anything else | one query, as `-q` takes it |

A query is prepared and run exactly as a `-q` would be, named `<query N>` by
the numbering of section 2.2, and its rows written to the output under the
same `-F`, `-s`, `-n` and `-o`, with the same separators: a blank line before
every query's rows but the first, counting the queries of phase 8. A query
must fit on one line. A line that is not a query, or not a whole one, is
refused with the parser's own diagnostic, and text left without a newline at
the end of input is a line like any other.

Only queries are accepted. A session cannot add facts or rules: a rule that
failed its check would leave the program rejected for the rest of the session,
since a check that has failed stays failed. Preparing a query adds nothing to
the program, so a mistyped query costs nothing but its diagnostic.

A line holding a NUL byte is refused: a query is text, and the bytes after a
NUL would be silently dropped.

### 10.2 Commands

| Command | Does |
| --- | --- |
| `.help` | lists the commands |
| `.plan QUERY` | runs QUERY and writes its plan instead of its rows |
| `.quit` | ends the session |
| `.stats` | writes the counters of `-X stats` |

A command's name is the text from the dot to the first blank. `.plan` takes
the rest of the line as its query; the others take nothing, and text after one
is refused. An unknown command is refused and names `.help`.

A command's reply is written to standard output, and never to the `-o` file:
the file holds query results and nothing else, and a reply answers the person
who typed the command.

`.plan` shows what running the query does, in the text of `-e`: over a program
not yet evaluated, the Magic rewrite and every component the query evaluates;
after `-a`, only the query's own plan. It is the query run, because a plan is
chosen when a query runs and the only faithful account of one is the one
evaluation gives. The plan is a reply and goes to standard output, under
`-e` too, where the plans `-e` writes go on to standard error. The query is
numbered like any other and counted under `-v`; its rows are discarded, and
its diagnostics point at the columns of the line as typed.

### 10.3 Prompts and line editing

When standard input and standard error are both a terminal, someone is taken
to be typing: the session prompts with `relic> ` on standard error, so that
standard output carries only what the session answers, and reads each line
with libedit where the build has it: line editing, and a history of the
session's lines. A history is not kept between sessions. Otherwise there is
no prompt and no line editing: a scripted session's output is its answers and
nothing else, and a session whose standard error goes to a file does not fill
it with prompts. Both read the same lines, and everything after reading a line
is the same code.

Everything answered is written out before the session waits for a line, the
rows of the queries of phase 8 included, so that whoever types a line has
seen the answer to the last.

### 10.4 Ending a session

A session ends at the end of input or at `.quit`, when a read fails, and
when a write fails, to the output or of a reply, since nothing it answers
after that could be seen. A query or
command that fails is reported and the session goes on to the next line.

The exit status is 1 when anything in the session failed and 0 otherwise, so a
scripted session reports its mistakes as a run of `-q` options would.

---

## 11. What is deliberately absent

* **Column headers.** A row's columns are the query's variables in order of
  first occurrence, which the person who wrote the query already knows.
  Headers would make every format two formats.
* **Decorative tables.** Architecture section 43.
* **Locale.** Nothing is formatted according to the environment.
