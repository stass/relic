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
* what `-s` sorts by and what `-o` does with a file.

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
9. **Close** the output, and report a write that failed.

Validation precedes evaluation throughout: a run either reports mistakes
without evaluating anything, or evaluates a program and a set of queries that
are all known to be valid.

### 2.1 Input

File operands are read in the order given and form one program. With no
operands, the program is read from standard input, under the name `<stdin>`,
provided standard input is not a terminal. With no operands and a terminal on
standard input the tool would wait, silently, for a program nobody is typing,
so that is a usage error instead; interactive use has an option of its own.

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

### 2.3 Combinations

`-V` stands alone. `-c` and `-a` contradict each other, since one forbids
evaluation and the other demands it. Either conflict is a usage error, as is an
unknown option, an unknown `-F` format, or a missing operand to an option that
takes one. Options that do not apply in a mode -- `-F`, `-o` and `-s` under
`-c` -- are accepted and have no effect.

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

## 8. What is deliberately absent

* **Column headers.** A row's columns are the query's variables in order of
  first occurrence, which the person who wrote the query already knows.
  Headers would make every format two formats.
* **Decorative tables.** Architecture section 43.
* **Locale.** Nothing is formatted according to the environment.
