# Command line

The `relic` tool loads a program, checks it, answers its queries and writes
the answers. It is a client of `librelic` and reaches the engine only
through `<relic.h>`, so anything the tool needs that the header does not
offer is a finding against the header. This document gives the order an
invocation runs in, which queries run and how they are named, why each
output format escapes what it does, how bytes reach the output and what a
failed write does, and how the interactive session behaves. The option
tables and every message are in `man/relic.1`; tables read with `-I` and
relations written with `-O` are [design-tables.md](design-tables.md). The
tool is the first interface over the library, and the reference client for
every other.

Related: [architecture](architecture.md#command-line),
[errors and exit status](architecture.md#errors-and-exit-status),
[library interface](design-api.md), [tables](design-tables.md),
[evaluation](design-eval.md), [planning](design-planning.md).

## Model

An invocation is a pipeline of phases over one context: parse the command
line into a configuration, load and import into the context, check, prepare
every query, then evaluate and write. Results go to standard output or the
`-o` file; diagnostics, plans, counts and counters go to standard error,
diagnostics in the library's
[diagnostic format](design-api.md#diagnostic-format).
The interactive session of `-i` is one more phase, answering lines read
from standard input into the same output as the queries before it.

## Invariants

* **Validate before evaluate.** A run either reports mistakes without
  evaluating anything, or evaluates a program, queries and exports that are
  all known to be valid.
* **Standard output, or the `-o` file, holds exactly the rows**, parseable
  in the format asked for. Nothing about the run -- a plan, a count, a
  diagnostic -- is written there.
* **Standard input has at most one reader**: the program, the session, or
  one import of `-`.
* **A write that fails is reported once and fails the run.**
* **Exit status is one of three**, whatever happened.

## Phases

Each phase runs only if the one before it succeeded:

1. **Parse the command line.** A malformed one is a usage error. The
   context's configuration is fixed here: `-l` sets its limits, `-X` its
   strategies, `-j` the size of its worker pool (with `-j 0` one thread per
   processor online).
2. **Load** every file operand, or standard input when there are none and
   nothing else reads it, then import every `-I` table in command-line
   order. Every file and table is attempted even after one fails, so one
   run reports the mistakes in all of them.
3. **Check** the program.
4. **Prepare** every query that is to run and every `-O` export's query,
   reporting what is wrong with each before any runs. A mistake in the third
   `-q` is reported even when the first two are fine.
5. With `-c`, stop: the program, its queries, its tables and its exports
   are valid.
6. With `-a`, **materialize** the whole program.
7. **Attach** the output.
8. **Run** each query in turn and write its rows. The first query that
   fails ends the run, because a failure in evaluation -- most often a
   limit -- would recur in every query after it.
9. **Export** each `-O` relation, in command-line order.
10. With `-i`, run the **interactive session**.
11. **Close** the output, and report a write that failed.

The output is attached after everything is validated, and the `-o` file is
created or truncated only once a query has been answered -- the library
delivers no row before its query's evaluation has succeeded -- or once a
run with no query succeeds, as an export's file is. A run that fails before
then, in validation or in evaluating its first query, at a limit included,
leaves an existing `-o` file alone; a run that fails later leaves it holding
what the queries before the failing one wrote. A run that succeeds leaves it
holding exactly its results, none included. A `-o` file that cannot be
opened is therefore reported once the first query has been answered.

No output -- the `-o` file, an export, or standard output when results or a
session's replies go there -- may be the same file as anything else the run
reads or writes. The rule, how a file is identified, and where it falls
short are in [design-tables.md](design-tables.md#writing-a-file).

`-c` contradicts `-a` and `-i`, which demand evaluation, and each pairing is
a usage error; `-V` stands alone. Options that do not apply in a mode --
`-e`, `-F`, `-o`, `-s` and `-v` under `-c`, `-F` and `-o` under `-n` -- are
accepted and have no effect.

## Standard input

File operands are read in order and form one program. With none, the
program is read from standard input under the name `<stdin>` -- unless the
session or an import of `-` claims it. Under `-i` standard input belongs to
the session and is never program text; the program is the file operands,
and with none it is empty. An import of `-` claims standard input the same
way. A second claim is a usage error naming both. The claim is on the
spelling `-`: `/dev/stdin` is a file like any other.

**A terminal is never read as data.** With no operands and a terminal on
standard input, the tool would wait silently for a program nobody is typing,
so that is a usage error; so is an import of `-` from a terminal.
Interactive use has an option of its own.

## Which queries run, and their numbers

With one or more `-q`, exactly those run, in command-line order; the
program's own queries are checked all the same, since they are part of the
program, but do not run. Without `-q`, the program's queries run in load
order. A program with no queries and no `-q` is checked and nothing is
evaluated; `-a` materializes it anyway, which is how to ask for evaluation
without a query -- to see whether a limit holds, say.

**Every query the tool runs has a number, counting from one**: the `-q`
options, or the program's queries when there are none, then each query line
of the session and each `.plan`, in the order read. Diagnostics about a
`-q` point at `<query N>`, so a message about the third says so; `-v`
reports a query by its number. A session line takes its number whether or
not its query can be prepared, so no two queries' diagnostics share a name;
a line refused before any query is read from it, such as an unknown
command, takes none.

**This numbering is the tool's, not the library's.** The `$q` name a plan
gives a query's answer relation is a serial the context assigns to every
query it compiles, the program's own among them, counting from zero, in the
order loads and preparations happen. The two answer different readers: the
tool's number matches what the user typed, the serial is unique within the
context's `-e` output.

## Output formats

A query's result is a set of rows of equal width. Every format writes one
row per answer and each value by its type. The exact byte forms are in
`man/relic.1`; what matters here is why each format escapes what it does.
Output is byte-transparent: csv and tsv write a string's bytes as they are,
and text and json write every byte either as itself or as an escape that
spells it, so nothing a string holds is lost.

**text** writes each value as a program would: an integer in decimal, a
symbol as its bytes, a string as a quoted literal, columns separated by a
space. Quoting strings keeps the format unambiguous: a symbol and a string
with the same bytes are different values and look different, and a space
inside a string is not a column break. The literal escapes every byte that
is not part of a printable character -- the C0 controls and DEL, a byte that
does not begin valid UTF-8, and both bytes of a C1 control (U+0080 to
U+009F), which some terminals obey as the start of an escape sequence just
as they do ESC. That keeps output **safe to print**: a program cannot send a
terminal escape sequence to whoever reads its results. Printable ASCII and
every other UTF-8 character are written as they are, so text in any script
reads as itself, and `\xHH` is an escape the language already has, so the
literal spells exactly the bytes the string holds.

**csv** is RFC 4180 with newline line ends; a field is quoted when it holds
a separator, a quote or a line end, or is empty, so a row of one empty
string is not an empty line some readers would skip. **tsv** escapes
backslash, tab, newline and carriage return instead of quoting, so neither
separator can appear inside a field. Both are untyped: a symbol and a string
with the same bytes are written alike. Both are machine formats and carry
control bytes unescaped, ESC included; printed to a terminal, an untrusted
program's results in either can carry escape sequences, as `cat` of the
program itself could.

**json** writes one document per query, an array of rows, each row an
array of values. An integer is a JSON number, written exactly; a symbol is a
JSON string of its bytes; a string is a JSON string holding the text
format's quoted literal. **A JSON string whose content begins with `"` is a
string value; any other is a symbol.** A symbol never begins with a quote,
so the convention is unambiguous, and a consumer recovers a string's bytes
by decoding the literal inside. The literal also solves a second problem: a
JSON document must be valid UTF-8 and a string's bytes need not be, and the
literal writes every invalid byte as `\xHH`, so the document is valid and
nothing is lost. A consumer that parses numbers as doubles cannot represent
every 64-bit integer and should read them as integers.

**A query with no variables** asks whether something holds; when it does,
its result is one row of no values -- an empty line, or `[]` in json.

**Several queries.** In text, csv and tsv a blank line precedes every
query's results but the first, whether or not either had rows. A session
that prompts and writes to a terminal leaves it out, since the prompt
already marks where results begin; written to a file or pipe the blank line
stays, since a later reader has no prompt to go by. In json each document
ends with a newline, so several queries are a sequence of documents a
streaming JSON reader consumes in turn.

There are no column headers: a row's columns are the query's variables in
order of first occurrence, which the person who wrote the query knows, and
headers would make every format two formats. Nothing is formatted by
locale.

## The row writer

The tool formats every row into a buffer of its own and hands the buffer to
the stream's descriptor with `write(2)` whenever the next byte would not
fit, so a block costs one system call rather than a stdio call per value,
whose locking would cost more than deriving the rows. A value is appended
by a formatter for its type, escaping as its format requires, and an
integer is rendered by a decimal routine of the tool's own. A block may end
at any byte, inside a value or an escape; the output is the same bytes
wherever blocks end, and a value longer than the buffer is written in as
many blocks as it takes.

**The block goes to `write(2)`, not `fwrite()`**, because on an unbuffered
stream some stdio implementations split a write into `BUFSIZ` pieces. The
stream itself -- standard output or the `-o` file -- has its stdio buffering
turned off with `setvbuf()`, so what other writers send through stdio (the
session's command replies) reaches the descriptor at once and in order with
the blocks; whatever stdio holds is flushed before each block in case
`setvbuf()` was refused. A short write is continued and one interrupted
before writing anything is retried.

**Everything the output writes goes through the buffer** -- rows, the
separators between queries, the json framing, sorted rows -- so the order of
bytes is the order of appends. The buffer is emptied when it fills and when
a query's results end, so it is empty between queries, and whatever follows
a query's results -- the next plan, a diagnostic, the `-v` count, a prompt
-- follows them on a terminal as in a file, without a list of places that
must flush.

**What a query writes first** -- its separator or opening bracket -- is
written only with its first row or once it has succeeded, and the library
delivers no row until everything that can fail has succeeded, so a query
that fails writes nothing at all.

## Write failures

A write can fail -- a full disk, a closed pipe -- and a tool whose output a
script consumes must not report success when it was cut short. Every write
is checked; the stream is flushed and closed explicitly, and each of those
is checked too.

**The first failed block is the one reported**, once, naming the output.
After it nothing more reaches the stream: later flushes drop what was
appended. The row being formatted is finished in the buffer, and the row
callback then returns nonzero, so the query's rows stop as an early stop
would; the query and the run fail with status 1. A closed pipe whose
`SIGPIPE` is ignored therefore costs one failed call and the formatting of
one row; otherwise the signal ends the tool at that write. A descriptor that
is full and set not to block fails like any other: the tool does not wait
for it to drain. A failure of the session's command replies, written
through stdio, is found by the stream's error indicator before the session
reads its next line, and reported the same way.

## Sorting

Result order is unspecified by the language, though deterministic for a
given input. `-s` collects each query's answer with `relic_query_collect()`,
sorts its rows lexicographically by column with `relic_result_compare()`,
and writes them through the same buffer. The order is the library's total
order -- integers before symbols before strings, then numerically or
bytewise -- so the tool has no second definition of it to drift. The sort is
a merge sort because the comparison needs the result to read a string's
bytes and `qsort(3)` gives its comparator no argument to carry it. Sorting
holds a query's rows in memory, which unsorted output does not. Under `-n`,
`-s` sorts only the exports' rows.

## Suppressed output, plans and verbosity

**`-n`** runs every query and writes none of its rows. The output is never
opened, so `-o` neither creates nor truncates its file. What remains is
standard error: diagnostics, plans, counts and counters. `-n -v` is how to
learn how many answers a query has without printing them, `-n -e` how to
see what a query would run.

**`-e`** writes the plan of everything evaluated to standard error, each
component's as it is planned and each query's before its rows. Standard
error, because a plan is about the run, not one of its results.

**`-v`** adds a line after each query that succeeded, `query N: M rows`,
under the answer it describes. Like `-X stats` it is data about the run
rather than a complaint, and is written without the program name, so its
text is the same however the tool was invoked. **`-vv`** asks the library
for its mixed-type notes ([diagnostics](design-api.md#diagnostics)): an
ordering comparison between values of different types is false and filters
its binding out, because a column may hold more than one type, and the note
keeps mixing nobody meant discoverable.

## The interactive session

`-i` reads standard input a line at a time and answers each line before
reading the next, so it works the same from a terminal and from a pipe, and
a scripted session is an ordinary way to use relic. A line is blank, a `%`
comment, a `.` command, or one query, prepared and run exactly as a `-q`
would be, into the same output under the same `-F`, `-s`, `-n` and `-o`. A
query must fit on one line. A line holding a NUL byte is refused, since the
bytes after a NUL would be silently dropped.

**A session accepts no facts or rules.** A rule that failed its check would
leave the program rejected for the rest of the session, since a check that
has failed stays failed, whereas preparing a query adds nothing to the
program, so a mistyped query costs nothing but its diagnostic. Nor can a
session import a table: a session's lines may come from a script, and a
command that named a file would let them open one, which program text
cannot do either ([tables](design-tables.md#program-text-cannot-name-a-file)).

**Commands** are `.help`, `.plan QUERY`, `.quit` and `.stats`. A command's
reply goes to standard output and never to the `-o` file, which holds query
results and nothing else; a reply answers the person who typed it. `.plan`
runs its query and writes the plan instead of the rows, because a plan is
chosen when a query runs and the only faithful account of one is the one
evaluation gives; its rows are discarded and it is numbered like any query.

**Prompts and line editing.** When standard input and standard error are
both terminals, someone is taken to be typing: the session prompts on
standard error, so standard output carries only answers, and reads lines
with libedit where the build has it. Otherwise there is no prompt and no
editing, so a scripted session's output is its answers alone. Everything
answered is written out before the session waits for a line, so whoever
types has seen the answer to the last.

**Ending.** A session ends at end of input, at `.quit`, when a read fails,
and when a write fails, since nothing answered after that could be seen. A
line that fails is reported and the session goes on; the exit status is 1
when any line failed, so a scripted session reports its mistakes as a run
of `-q` options would.

### Ctrl-C

**SIGINT is caught only while a prompting session answers a line**, since
only there is someone to return to. The handler calls `relic_interrupt()`
on the context, records the interrupt for the line, and writes a newline to
standard error so the diagnostic does not begin on the line the terminal
echoed `^C` on; each is async-signal-safe. The interrupted query fails with
its diagnostic and the session reads the next line. Rows written before the
interrupt stay written and end at a row boundary; the query's framing is not
completed -- json lacks its closing bracket -- and `-v` does not count it.
The previous disposition is restored after each line, and a SIGINT the tool
was started ignoring stays ignored.

The record covers what the line does outside an evaluation, since the
library forgets an interrupt made between operations
([interruption](design-api.md#interruption)): a line interrupted while its
query was being prepared fails there rather than running, and under `-s` the
tool, which writes the collected rows itself, looks at the record before
each row. An interrupt in the library's window -- after the tool's last look
and before the run enters, a few instructions of a call -- is forgotten, and
the query runs until a second Ctrl-C.

**The handler restarts the system calls it interrupts**, so a write of the
session's output or of a diagnostic is not cut short. The consequence is
that a session blocked writing to a pipe its reader stopped draining stays
blocked, and Ctrl-C does not return it to the prompt. Ending the write
would mean telling every write that may be cut short from those that may
not, for a case where the reader, not the query, stopped the session.

**Ctrl-C at the prompt is not caught.** A line editor reading the terminal
does not reliably give up its read when a signal arrives: libedit retries
the read once, then reports end of input, and its own handling of a signal
it survives can leave the terminal in the mode it reads in. So while the
session waits for a line, SIGINT keeps the disposition the tool started
with, which by default ends the process with the terminal restored. So does
Ctrl-C during the loads and `-q` queries before the session, throughout a
run without `-i`, and in a session whose standard input or standard error
is not a terminal: a run with no one to return to ends.

## Exit status

| Status | Meaning |
| --- | --- |
| 0 | every phase succeeded, or `-V` printed the version |
| 1 | the program, a table or a query was rejected; evaluation failed; anything in an interactive session failed; input or output failed; or the library could not start, for want of memory or of a random device |
| 2 | the command line was malformed, or it would have a program or a table read from standard input while that is a terminal |

Three statuses, so a shell script can branch on them; `<sysexits.h>` does
not reach the interface. A query with no answers is a success, and warnings
do not change the status. An exceeded limit is status 1, and the query that
exceeded it writes no rows. The tool
never reads the library's error class: every failed library call is 1, and
a usage error is one the tool finds in its own command line before calling
the library.

## Options

Options are single letters parsed by `getopt(3)`, in BSD style; there are
no long options. Files are operands, not arguments of a file option.

**`-l key=value`** sets a resource limit and is repeatable, rather than
one letter per limit, so new limits never consume option letters. Each key
fills the corresponding field of the library's configuration, so a limit
exists in one place and the tool merely parses into it.

**`-X name`** selects an internal behaviour: an alternative evaluation
strategy, or counters printed to standard error when the run ends. It is
documented in `relic(1)` but is not a stable interface. It exists so that
the differential suites compare strategies through the ordinary command
line rather than through a private test entry point; each strategy must
produce identical answers ([testing](design-testing.md)). `-j` and `-X
parallel` change nothing a run writes except the shared-rounds counter of
`-X stats`.

## Determinism

The same program, tables, options and session input produce the same
output bytes and the same diagnostics in the same order at any `-j`
([Determinism](architecture.md#determinism)). Query numbers are a function
of the command line and the session's lines; result order is the library's
deterministic order, or the total order under `-s`. An interrupt is the one
timing-dependent input, and it only shortens a line's output at a row
boundary.

## Alternatives not taken

* **Writing through stdio alone.** `fwrite()` on an unbuffered stream may
  split each block into `BUFSIZ` writes, and a buffered stream would
  interleave out of order with the replies written through stdio.
* **Column headers and decorative tables.** Every format would become two;
  the default output is meant to feed `awk`, `cut` and `join`.
* **Accepting rules or facts in the session.** A rule that failed its check
  would poison the rest of the session.
* **Catching SIGINT at the prompt.** libedit cannot abandon its read
  reliably.
