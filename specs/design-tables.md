# Tables: import and export

Data arrives as tables -- a database or spreadsheet export, the output of
`awk`, `cut` and `join`, relic's own `-F csv` and `-F tsv` -- and results
leave as them. relic reads a csv or tsv table as the facts of one predicate
(`-I`, and `relic_import_file()` / `relic_import_path()` in the library) and
writes a whole relation to a file (`-O`). The two are one subject: they are
named by one operand grammar, each format read is the exact inverse of the
format written, and neither can be named from program text. The language
does not change: an imported fact is exactly the fact the equivalent text
would state, and an exported relation is exactly the answer to a query.
Tables are an interface at the edges of the pipeline: an import feeds the
facts, an export reads what evaluation derived.

Related: [architecture](architecture.md#tables), [facts](design-facts.md),
[command line](design-cli.md), [library interface](design-api.md),
[values and types](language.md#values-and-types),
[testing](design-testing.md).

## Model

Without an import, a table becomes facts by rewriting each row as
`p(a, b).`, usually with a line of `awk`, and the rewrite is where the
mistakes are made: a field with a space, a capital first letter, a quote or
a backslash must become an escaped string literal, and a script that
forgets writes a program that does not parse, or one with a symbol where a
string was meant, which parses and joins with nothing. The engine already
knows how to spell values, so the format and column types define their
meaning without an external conversion script. Import construction and
parallelism are specified in [bulk loading](design-bulk-load.md).

An **import** names a format, whether the table has a header, a predicate,
a type for each column (or `_` to skip it) and a source. The library reads
the external stream once, using a pure state machine for record framing and
decoding. It preserves scalar field-event semantics through admitted batch
windows or scalar replay, stages each new tuple, and commits the whole
table through the commit every load uses.

An **export** names a format, a predicate (with its arity when the name has
several) and a file. Its contents are the answer to a query of the
predicate with a distinct variable in every column, written by the tool's
row writer into the file.

## Invariants

* **Only the caller names files.** No statement of a program opens or
  writes one.
* **Nothing about a type is inferred.** Every kept column's type is
  declared; every field that does not fit its type is an error.
* **Each format read is the exact inverse of the tool's output format of
  the same name**: one query's `-F csv` or `-F tsv` output reads back under
  its columns' types as the facts it came from.
* **An import is all or nothing**, and its first mistake ends it.
* **An export writes exactly what a query of all its columns writes**, in
  the query's order.
* **The reader alone decides where fields and records end**, from its state
  and the next byte.

## Program text cannot name a file

Souffle's `.input`/`.output` and Nemo's `@import` let a program name its
files. relic does not, for three reasons.

* **Program text is untrusted.** relic is designed on the assumption that a
  program may come from someone else: the fuzz targets take arbitrary
  programs, the hash seed is random so no program can be crafted against
  it, and text and json output escape the control bytes a program's strings
  hold, as csv and tsv do at a terminal. Loading a program gives it no
  capability but computation. A directive that opened a path would let a
  program read any file the user can read and print it as rows -- a
  private key among them -- and one that wrote a path could overwrite any
  file the user can write. With files named only by whoever runs the tool
  or embeds the library, a program opens nothing.
* **The program stays a function of its text.** Data named by the caller
  is an argument, as input is to every other Unix filter. A program that
  named its own files would mean something different on every machine.
* **The grammar stays about relations.** A directive would be the grammar's
  first statement about the environment, added for something the caller can
  already say.

The price is that a program does not carry its data's location; the
command line, or the script around it, does. The interactive session
names no files either ([command line](design-cli.md#the-interactive-session)).

## Naming a table

### One operand grammar

`-I` and `-O` share one spelling, parsed by one piece of the tool
(`cmd/relic/operand.c`), and `-T` declares a predicate's column types:

```text
types   := predicate ":" columns
import  := path | format [ ",header" ] ":" predicate [ ":" columns ] "=" path
export  := path | format ":" predicate [ "/" arity ] "=" path
columns := column ( "," column )*
column  := "symbol" | "string" | "integer" | "_"
```

```sh
relic -T edge:symbol,symbol -I edge.tsv graph.dl
relic -T person:symbol,string,integer -I csv,header:person=people.csv rules.dl
relic -a -O path.tsv -O dl:path=closure.facts tc.dl
```

An operand that holds a `=` has a head, and its path is everything after
the first `=`; nothing in a head can hold one, so a path may. Everything but
the path is fixed vocabulary and a predicate name, so a malformed operand is
a usage error, status 2, found before anything loads. Each option is
repeatable, one table or one declaration per operand, as a `-l` operand is
one limit.

**A path alone names its format and predicate.** The file name's
extension is the format and the rest of the file name is the predicate:
`-I data/edge.tsv` imports `edge` as tsv, `-O out/path.dl` exports `path` as
facts. A path whose file name has no known extension, or whose stem is not a
predicate name, is a usage error that asks for a head, as is a path holding
a `=`. **A format is named, never guessed from the contents**: both the
head's word and the extension are written by the caller. A pipe or
`/dev/fd/3` has no extension and is named with a head, and a file is read
in the format its name says whatever its bytes look like. A path alone
imports no header, so a table with one is named with a head.

**Column types belong to the predicate, not the file.** Every table
imported into one predicate fills columns of the same types, so `-T` gives
them once, and an import that lists none takes them from its predicate's
`-T`, given anywhere on the command line. An import may list its own for a
table laid out differently -- which fields a table skips with `_` is that
table's layout -- but every list the command line gives one predicate must
keep the same types in the same order, or the run is a usage error; so is a
second `-T` of a predicate, and an import with no columns and no `-T`. A
`-T` no import uses is fine, so a script can declare a project's predicates
once and import some on each run. Types are words, not letters: the mistake
a declaration can make is declaring as a string a column the program
joins as symbols, and a letter `s` would invite it from either side.

**An export's arity is named only when the name leaves it open.** A
predicate is a name and an arity together. An export without `/arity`
exports the one predicate the program has of that name, and is an error
when it has several; the arities come from `relic_predicate_arities()`,
since only the library holds the predicate table. A name the program does
not know at all is an error, most often a misspelling. These errors depend
on the program, so they are reported when queries are prepared, status 1.

**`-` is standard input for an import, and never a path for an export.** A
relation written to standard output is a query's answer, which `-q` and
`-F` already write, and a second writer of standard output would interleave
with the query rows; a file named `-` is spelled `./-`.

### Through the library

The library takes an import's description as a complete type the caller
fills in; the words above are the tool's spelling, and the library has none
of its own (see `include/relic.h`). **Its enumerators start at one**, so
a description or column array left zeroed -- by `calloc(3)`, or a forgotten
field -- is refused rather than read as csv of symbols. An unknown flag is
refused rather than ignored, for the reason
[design-api.md](design-api.md#facts-and-imports) gives.

The description and its column array are borrowed for the call and read
once, when checked, since caller code -- a stream's read function -- runs
while the table is read. Both entry points enter the context as a load
does, so that read function, and the diagnostic callback, find it busy. An
import adds facts and nothing else, so it needs no check afterwards and an
embedder may import between queries.

## Column types

A `symbol` column accepts what the lexer's symbol rule accepts, an
`integer` column `-?[0-9]+` within `int64_t`, and a `string` column any
bytes at all, none, NUL and bytes above 0x7f included; a `_` column accepts
whatever the format can hold and becomes nothing.

**Nothing is inferred.** The symbol `ann` and the string `"ann"` are
different values, and a join between them fails without a word. Each way
of not declaring types would let that failure pass silently. Guessing from
the bytes makes whether a join fails depend on the data: a column of names
would import `ann` as a symbol and `Ann` as a string, and join on one and
not the other. Reading each field as a constant in program spelling needs
data written in relic's quoting, which ordinary csv is not -- and relic's own
csv output is not either, since csv is untyped. A declared type moves the
decision to one place, once per column, and `Ann` in a symbol column fails
at its line.

**A field is typed after its format decodes it**, since quotes and escapes
belong to the format, not the value: `"7"` in a csv integer column is 7.
**Symbol and integer mean what they mean in a program**: the import calls
the lexer's own symbol and integer functions over the whole field, so `7 `,
`7x` and `1-` are not integers, `007` is 7, `not` is a symbol value, and a
column of codes whose zeros matter is a string column. **No whitespace is
trimmed**: RFC 4180 counts spaces as part of a field, and trimming would
change what a string column holds. A field's value is interned as a fact's
constant is, so an imported `a` is the same value as the `a` of a rule and
the two join.

**A skipped field is read, not decoded into a value.** The format's quoting
and escapes hold for it, so a record's structure does not depend on which
columns are kept, but nothing of it is interned. `_` is spelled as the
language's anonymous variable because it does the same thing. It exists
because tables are often wider than the relation a program wants: importing
every column and projecting with a rule would intern every column's values,
and a table wider than the arity limit could not be imported at all.

## The formats

Each format is read as the exact inverse of the tool's output format of the
same name ([output formats](design-cli.md#output-formats)), and reads what
other tools write wherever that does not conflict with being that inverse.
The output of several queries is not one table: the blank line between them
would read as a record.

**Records, in both formats.** Outside a quoted csv field a record ends at a
line feed or a carriage return and line feed, so a file written on Windows
reads as one written anywhere else. The last record needs no line end, and
a final newline does not make an empty record. An empty input holds no
records and commits nothing. An empty line is a record of one empty field:
the row of one empty string, and also how both formats write a row of no
columns, which an import of one `_` column reads back. Every record has
exactly as many fields as the import has columns, skipped ones included.
Bytes are data: no encoding is assumed, no byte-order mark removed and no
locale consulted.

**tsv.** Fields are separated by one tab. Inside a field there are exactly
four escapes, `\\`, `\t`, `\n` and `\r`, the four the output writes. A
backslash before anything else, or ending a field, is an error rather than
a literal backslash, for the reason the lexer refuses unknown escapes: once
`\q` imports as two bytes, giving it any other meaning later changes what
existing files import as. A raw carriage return not ending a line is an
error, since the output writes `\r` and a raw one is either a file whose
lines end in a lone carriage return or a byte a producer failed to escape.
So a file whose producer does not escape backslashes -- a Windows path, or a
PostgreSQL `COPY` file using `\N` -- is refused at its first such backslash.
That is the price of reading relic's own output back exactly: a reader that
took backslashes literally could not recover a tab or newline inside a
string.

**csv.** RFC 4180, as the output writes it. A field beginning with `"` is
quoted and ends at the next `"` that is not half of a doubled `""`; inside
it, commas, carriage returns and line feeds are data, and after the closing
quote the field must end. A `"` inside an unquoted field is an error,
because RFC 4180 requires such a field to be quoted and a reader that let
one through would have to guess what `a"b,c"` meant. A stray carriage
return is an error, as in tsv. `""` and an empty unquoted field are both the
empty string: relic has no null for the distinction to map to.

**The header.** With the header flag the first record is skipped. It must
have as many fields as the import has columns -- the cheapest sign that a
declaration is wrong -- and is not typed or interned; a header flag over an
input with no records is an error. Without the flag the first record is
data: over an integer column a header fails loudly, over symbol and string
columns it would import as a fact. Detecting a header would mean inferring
something about the table from its data, which the import refuses to do.

## The reader

**One state machine alone decides where fields and records end.** Its
state says where it stands, and each byte moves it to the next state;
every boundary follows from the state a byte arrives in. A line feed outside
a quoted field ends the record, and inside one is data. Nothing else -- no
second pass, no count of quotes -- decides whether a field is open, so a
misplaced byte is an error in the state it arrives in, and a stray quote or
lone carriage return is reported when it is read, not after the lines that
follow it have been gathered.

**Each state defines what the end of input means there**, since the end is
not a byte:

| State | End of input |
| --- | --- |
| start of a record | ends the table; there is no record |
| start of a field, after a separator | an empty last field; ends the record |
| inside an unquoted field | ends the field and the record |
| inside a quoted csv field | an unterminated quoted field |
| after a quote inside a quoted field | the quote closed the field; ends it and the record |
| after a tsv backslash | a backslash at the end of a field |
| after a carriage return that needs a line feed | a carriage return inside a field |

**The reader is pure and event-driven.** It is a function of its state and
the next bytes, with no input, output or context, and emits three events: a
field ended, with its decoded bytes; a record ended; a mistake, at its
location. So it reads a table identically whether the table arrives whole
or a byte at a time, and unit tests drive it directly over byte strings fed
both ways. The importer consumes the events in order: it types each field
as it ends -- interning it, or dropping it when skipped -- and stages the
tuple when the record ends. Because events are consumed in order, a mistake
the importer finds in a field is met before anything the reader would find
after it.

**Scalar reading holds one field and one tuple.** A field's decoded bytes
accumulate in a reused buffer, which the reader grows in one place, where
it holds the field to `max_input` (see [failure paths and
limits](#failure-paths-and-limits)). Batch import additionally holds a bounded
raw-input window, piece dictionaries and record tokens. Its framer and
decoders use the same parser transitions. An oversized record switches to
scalar continuation through buffered input, so batching never requires the
whole file or an arbitrarily large raw record in memory.

## Staging and the commit

An import is a load whose text is a table. It accepts facts in input order
and, when the whole input has been read, commits them through the
commit of [facts](design-facts.md):

* **All or nothing.** A read error, a malformed record, a field its column
  refuses, or a limit adds no fact; the base stores stay as they were.
* **Limits refuse while staging.** Each new tuple is measured against the
  tuple and memory limits as it is staged, exactly as a loaded fact is, so
  an import the limits cannot admit stops at the record that shows it,
  reporting a load's message,
  `relic: error: limit exceeded: tuples=N while loading edge/2`, with
  nothing grown. The commit measures exactly and can refuse what staging's
  floor let through. Staging itself is charged to no
  limit: it is proportional to the input the caller chose.
* **Duplicates are held once**, within an import and against the facts a
  base store already has. An import that adds nothing new discards nothing
  derived; one that adds anything discards everything derived, as any new
  fact does.
* **What a failed import interned stays interned.** The predicate is
  interned before the table is read. Values consumed before failure remain
  in the context even though the import adds no facts, as with a failed load.

A batch window publishes values only when its final monotone projections
prove that every scalar field and staging prefix is admissible. An
unproved window replays from the last accepted boundary through the scalar
consumer before reporting an input or limit error. This preserves the
refused record's value effects and prevents values from later records
being interned. Base facts still publish only after the complete input
and, for an owned path, its close succeed. See the
[window proof](design-bulk-load.md#proving-a-whole-window-admissible).

An import that commits nothing defines nothing: a predicate is defined when
its base store holds tuples, so a program reading a predicate imported from
an empty table draws the undefined-predicate warning, which is accurate.

## The first mistake ends an import

The parser reports every broken statement, because each is a separate
mistake an author made. A table is usually written by a program, and its
mistakes are systematic -- a wrong delimiter, columns declared in the wrong
order, a header without the flag -- so one mistake breaks every record, and
a million identical messages help no one. An import stops at its first
mistake and reports only that one. The tool attempts every import and
every file, so one run reports the first mistake in each.

**The first mistake is the first the reader meets reading forward.** A
field's bytes are checked as read, its type when it ends, a record's width
when a field beyond the last column begins or when the record ends short.
The scalar consumer's forward event order determines the reported mistake.
That is also why a record with too many fields does not say
how many: counting them would mean reading on through bytes that may hold
mistakes of their own -- a quote opened in an extra field can run to the end
of the input.

Parallel pieces report no diagnostic themselves. A malformed or refused
window follows the scalar replay above, using the same field/record event
order. Read-ahead errors remain queued behind earlier successful reads;
the bytes a failed read returned are dropped rather than treated as
successfully supplied.

**Every mistake has a location**: the source name, and the line and column
of the byte to look at, counting bytes from one, as the lexer's do. A line
is a physical line, so line feeds inside a quoted csv field advance it, and
counts saturate at the largest value a location holds rather than wrapping
to an earlier line. A failure to read has no location and names the
system's reason. The wording of messages is not a contract; `man/relic.1`
shows them.

**A message is printable ASCII with no backslash.** It goes to whatever
diagnostic callback is installed, which may show it without escaping, so a
table's bytes must never reach it raw; and the tool's own formatter doubles
backslashes, so one in a message would misstate the table. A field is
quoted in a message when all its bytes are printable ASCII other than a
quote or backslash, cut past a few dozen bytes as the parser cuts what it
quotes; any other field is named by its first byte that is not, as a hex
value. Nearly every real mistake -- `Ann`, `x`, `7.5` -- is quoted in full.

## Export

**An export's contents are the answer to a query of all its columns.**
`-O path.tsv` writes exactly the rows `-F tsv -q 'path(V1, V2)?'` writes.
The tool prepares that query, named `<export N>` by its position among the
`-O` options, and runs it, so an export reads a relation exactly as a query
does -- the materialized relation under `-a`, otherwise through the Magic
rewrite -- and writes through the same row writer, buffer, sort and failure
contract ([the row writer](design-cli.md#the-row-writer)). The tool has one
way to read a relation and one way to write rows. Each export is its own
query, so a run exporting several predicates does well to materialize first
with `-a`.

**No header is written.** Columns are positional and have no names, and a
header of types would be a row of symbols that an import would read as
data. What a later import reads a column as is that import's `-T`.

**Formats.** csv and tsv are the output formats, and an import reads them
back; being untyped, a column that mixes a symbol and a string with the
same bytes cannot be recovered from them. json is the output's document,
which keeps every value's type, for consumers other than relic. **dl**
writes each row as a fact of the exported predicate -- `path(a, "Ann
Lee").`, or `done.` for no columns -- each value spelled as the text format
spells it, which is the language's own spelling. **A dl export is a program
and loads back as one**: named as a program operand in a later run, it
holds exactly the relation exported, mixed columns included, with no `-T`.
It is loaded, not imported, because a file of facts is the language
already. Its literals escape every control byte and invalid UTF-8 byte, so
a dl export is safe to print and valid UTF-8. Only an export offers dl:
a query's rows are not facts of any predicate.

`-s` sorts an export's rows, `-e` writes its plan, `-v` writes `export N: M
rows` after it, and `-F`, `-n` and `-o` do not apply to it.

## Where tables fall among the phases

Within the tool's [phases](design-cli.md#phases):

* **Imports load after the program**, in command-line order, so a run's
  diagnostics come in the order a reader looks for them -- the program's,
  then the data's. The order changes no answer: nothing is evaluated until
  every load and import has returned. Every import is attempted after a
  failure, and `-c` reads, splits and types every table, so a table that
  does not match its declaration is reported without evaluating anything.
* **Standard input** follows the tool's rules
  ([standard input](design-cli.md#standard-input)): an import of `-`
  claims it, and a program piped in is read whether or not `-I` is given,
  so none is dropped silently. A table is queried with no program by
  importing it from `-`, or by naming `/dev/null` as the program.
* **Exports are resolved with the queries and written after them**, in
  command-line order, so the answers a person waits for come first; and
  before the session, so what an export holds does not depend on what the
  session did. A session adds nothing to the program but may hit a limit,
  and an export written before it is complete whatever it meets. The first
  export that fails ends the run.

## Writing a file

**Every file a run writes is opened before anything is read**, the `-o`
file and each export's, for writing, created when it does not exist and
truncated by none of it. Opening first is what lets a run tell which file
each output is from the file it has open (below), and reports a file that
cannot be opened, in a missing or read-only directory, before any query
runs. `-c` writes nothing and opens no output; `-n` opens no `-o` file.

**A file is truncated when its first row is written**, or once its query
has succeeded with no rows, which leaves it empty. The library delivers no
row until everything in the evaluation that can fail has succeeded, so an
export whose evaluation fails -- at a limit, for memory -- leaves an
existing file as it was, as does a run that fails before exporting. The
file is cut where its descriptor stands, which is its start for every file
the run opens afresh. On macOS and the BSDs a path naming a descriptor,
such as `/dev/stdout`, opens the descriptor's own file description, whose
offset is past whatever was written before the run; cutting it at its start
would destroy that and leave a hole of zero bytes up to where the rows are
written, so it is cut at its offset, and not at all when it is open for
appending.

**A file the run did not write is closed when the run ends**, after
everything else: one that existed is as the run found it, and one this run
created is removed, so a failed run leaves no empty files behind. Only an
`O_EXCL` open is taken to have created a file, since it succeeds only when
it is what creates it. A path that exists but does not open without
`O_CREAT` is a symbolic link to a file that does not exist; the link is
followed by hand and the name it gives is created with `O_EXCL` in turn, so
a file another process creates there meanwhile is opened as existing rather
than taken for the run's. The file is removed by the name the path resolves
to, since `unlink(2)` of a symbolic link would remove the link. That name's
directory is opened, and the name compared and removed within it, only when
it is still the file the run has open, so neither a file another process put
there since nor a directory replaced by a link meanwhile sends the removal
to a file the run did not create. A run ended by a signal before it writes
leaves a file it created there, empty. What can fail after the first row is
the writing itself, reported naming the file, and an interrupt, which
outside a session ends the process; either leaves the rows written before
it. Closing a file whose writes succeeded can fail, and that fails the run,
reported once whichever part of the tool closes it.

**Opening an output never waits.** It is opened with `O_NONBLOCK`, cleared
once it is open, and with `O_NOCTTY`, so a terminal named as an output does
not become the process's controlling terminal. A FIFO that no process reads
yet does not open without blocking, which `stat(2)` tells apart from another
file refusing the same way; it is opened when the first row is
written to it, which waits for a reader there, as writing to it would
anyway. A run that fails before then never waits for one. Such a FIFO has no
identity, like any FIFO, and a regular file found at its path by then could
be one the run reads, so it is refused.

**There is no atomic rename.** Writing a temporary file and renaming it
over the target would spare a reader the partial file, at the price of
replacing a symbolic link rather than writing through it, and of the mode
an existing file had. The tool writes every file as `-o`
does, following symbolic links, with the mode `fopen(3)` gives a new file,
and the exit status is what says whether a file is whole.

**No output may be the same file as anything else the run reads or
writes.** The outputs are the `-o` file, every export, and standard output
when results or a session's replies go there; the inputs are the program
operands, the imported tables, and standard input when the program, a table
or the session reads it. Every output truncates its file, so one that is an
input would replace it -- `-o q.dl q.dl` would replace the program with its
own answers -- and one that is another output would destroy what that one
wrote. The command line is refused as a usage error, before anything is
read or written, naming the output and the file it is; the outputs opened
for the check are closed as a run that wrote nothing closes them.

**Files are compared by identity, not by spelling**, in one check over every
pair of an output with any file listed before it, so that each new kind of
input or output joins one list rather than a set of pairwise rules. A
regular file is its device and inode, which `x`, `./x`, a symbolic link and
a hard link all share. An output's are read by `fstat(2)` of the descriptor
it was opened at, and standard input's and output's of theirs, so what is
compared is the file each one writes or reads, not a prediction of it. A
file that did not exist before the run has its identity too, because
opening it created it, once, whatever names lead there: `-o x` and
`-O dl:p=./x`, a link to a directory and the directory, a symbolic link
whose target did not exist and the target, and on a file system that
ignores case or normalizes Unicode, two names that differ only in that,
are one file. An input, which the run opens only later, is identified when
the outputs have been opened: by `fstat(2)` of the file its path opens,
once `stat(2)` says it is a regular file, opened for reading without
blocking, so opening it has no effect of its own. Opening the path, rather
than only `stat(2)` of it, is what makes `/dev/stdin` and `/dev/fd/0` the
files their descriptors hold: on macOS `stat(2)` of such a path gives the
inode with the device of `/dev`. An input that names an output's new file
is that file, and is refused. Anything else has no identity and conflicts
with nothing: a device or a FIFO holds no stored data an output could
replace, so any number of outputs may write `/dev/null`; a directory fails
as an output when it is opened, and an input that does not exist or cannot
be examined fails when it is read.

The check is a guard against mistakes on the command line, not a security
boundary. Every output is the file the run has open, but an input is
identified before it is read, so a file another process links or renames
over an input in between is not seen.

## Failure paths and limits

A table is input the caller chose, as a load's text is, and is treated the
same way, including when it comes from an untrusted party.

* **The arity limit holds**: at most 32 columns are kept. Nothing else about
  a table's shape is bounded.
* **The tuple and memory limits bound what an import adds**, refusing at
  the record that shows the table cannot fit. What was interned before the
  refusal stays for the life of the context, which an embedder feeding one
  long-lived context untrusted tables must allow for; a load has the same
  property. The reader may have read a buffer of the stream past the
  record that ended the import.
* **The input limit bounds the field buffer.** A field whose decoded
  bytes would pass `max_input` is refused as it grows past it, with
  `limit exceeded: input=N while importing t.csv` located at the field's
  first byte -- for a quoted csv field, its opening quote. So a quoted
  field left open early, which may hold line feeds and would otherwise
  hold the rest of the input, holds at most the limit. Every field is
  held to it, a header's and a skipped column's included, since the reader
  holds each one's bytes whatever its column does with them. The field
  buffer is sized by the input, not by what the program stores, so it is
  not charged to `memory`, a ceiling on accounted storage and evaluation
  memory rather than on the process
  ([the memory limit](design-memory.md#the-memory-limit)); the input limit
  is its bound, and with none set the longest field is. A record is not
  held as a whole -- each field is typed and interned as it ends -- so a
  record needs no bound of its own.
* **Crafted collisions gain nothing.** Staging, the symbol pool and the
  value table hash with the context's random seed.
* **A string column holds any bytes**, which text and json output escape
  when printed and csv and tsv write as they are.
* **Only the caller opens files**, and an export writes only files the
  command line names.

## Determinism

Imports run after the program in command-line order; an import interns its
predicate first and then its values in order of first appearance, and each
base store's new tuples are appended in table order; the mistake reported
is the first the reader meets. An export writes the same bytes for the same
program, data and options at any `-j`, because its rows are a query's, in
the query's deterministic order or sorted, and exports run in command-line
order. So two runs over the same program and tables produce the same
identifiers, plans, rows, files and diagnostics.

## Alternatives not taken

* **Import or export directives in program text**, for the reasons above.
* **Type inference**, or reading fields as program constants.
* **Other delimiters and quote characters.** Each format is the fixed
  inverse of an output format; another would be a new format word.
* **Columns chosen by header name.** Columns are positional, as the
  output's are, and `_` already says which to skip.
* **Continuing past the first mistake.**
* **Committing in batches.** It would bound staging, but a failed import
  would leave part of a table behind, and each batch would discard what had
  been derived.
* **A header on export, exporting every predicate, or text as an export
  format.** Columns have no names; a program's relations include ones used
  only on the way to its answers; and dl is the spelling that reads back.
* **Writing a file atomically**, for the reasons above.
