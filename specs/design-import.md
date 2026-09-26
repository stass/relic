# relic table import design

**Status.** Design document for reading facts from CSV and TSV tables: how
an import is named on the command line and through the library, how each
field becomes a value of the type its column declares, the two formats as
they are read, and how an import reaches the base stores. The language does
not change: an imported fact is a fact, exactly as though the program had
stated it. Forward-looking, like every document under `specs/`; `STATUS.md`
records what is implemented.

**Companions.** `specs/design-facts.md` sections 4 and 13, the staging and
the commit an import feeds; `specs/design-cli.md` sections 2 and 4, the
phases of an invocation and the csv and tsv output this reads back;
`specs/design-api.md`, the entry points, re-entry and versioning;
`specs/design-parser.md` sections 3, 4 and 7, the symbol and integer rules,
locations and diagnostics; `specs/design-hardening.md` section 5, the fuzz
targets; architecture sections 10.1, 40, 48, 50, 63, 67 and 69.

---

## 1. What this document settles

* how an import is named: a format, whether the table has a header, a
  predicate, a type for each column and a source -- one `-I` operand on the
  command line, one `relic_import_t` through the library;
* why program text cannot name one;
* what each column type accepts, and that nothing is inferred;
* the tsv and csv formats as read, each the inverse of the tool's output
  format of the same name;
* the header;
* how an import is read, in one pass over its bytes, and staged and
  committed, all or nothing, through the commit a load uses;
* what is reported, where, and why the first mistake ends an import;
* where imports fall among the tool's phases, and who reads standard input;
* the tests, the fuzz target and the order of work.

---

## 2. Why

Data arrives as tables: exports from a database or a spreadsheet, the output
of `awk`, `cut` and `join`, and relic's own `-F csv` and `-F tsv`. Without
an import, a table becomes facts by rewriting each row as `p(a, b).`, usually
with a line of `awk` such as `source_facts` in `bench/datasets.sh`. The
rewrite has to spell values as the language does. A field with a space, a
capital first letter, a quote or a backslash must become a string literal
with escapes. A script that forgets writes a program that does not parse,
or one with a symbol where a string was meant, which parses and joins with
nothing. The engine already knows how to do the rewrite, and the rewrite is
where the mistakes are made.

An import also costs less than the text it replaces, though not by much. At
`facts-join` scale 4, a ternary fact loaded as text costs 165 to 199 bytes
at the resident peak over ten direct runs (`TODO.md`). Of that, 24 bytes are the text itself,
read whole and held until the load returns. The facts reach the same base
stores either way, so the rest is the same: 86 bytes for the values the
constants intern, about 30 for the staging set, 21 for the tuple and its
index slot, and the slack of the arrays that grow by doubling. An import
holds no text (section 6.1). Splitting a record is a simpler scan than
lexing a fact, but interning dominates both. The gain is about an eighth of
the peak, not a change in kind. Interoperability is the reason for the
feature.

---

## 3. Naming an import

An import names five things:

| Part | Says |
| --- | --- |
| format | `csv` or `tsv` |
| header | whether the first record is a header, to be skipped |
| predicate | the predicate each record becomes a fact of |
| columns | a type for each field of a record: `symbol`, `string`, `integer`, or `_` to skip the field |
| source | a file, or standard input |

The predicate's arity is the number of columns that are not skipped. A
predicate is identified by its name and arity together, so an import of
three kept columns into `edge` adds to `edge/3`, whatever `edge/2` holds.
When a program reads `edge/2` and nothing else defines it, the
undefined-predicate warning names it, which is how a column list one column
too long or too short usually shows itself. It is a warning, and it does not
fail the run.

An import whose every column is `_` has arity zero. It holds its one fact
when the table has a record. An empty line is how csv and tsv write a row of
no columns (`specs/design-cli.md` section 4.5), and an import of one `_`
column reads that row back.

### 3.1 On the command line

```text
-I format[,header]:predicate:column[,column...]=path
```

```sh
relic -I tsv:edge:symbol,symbol=edge.tsv graph.dl
relic -I csv,header:person:symbol,string,integer=people.csv rules.dl
cut -f 1,3 access.tsv | relic -I tsv:hit:symbol,integer=- rules.dl
```

The operand's grammar:

```text
import     := format [ ",header" ] ":" predicate ":" columns "=" path
format     := "csv" | "tsv"
columns    := column ( "," column )*
column     := "symbol" | "string" | "integer" | "_"
predicate  := [a-z][A-Za-z0-9_]*, other than "not"
path       := one or more bytes; "-" is standard input
```

`-I` is repeatable, one import per operand, and the operand is
self-contained, as a `-l` operand is one limit. The path is everything after
the first `=`. Nothing before that `=` can contain one, so a path may.
Everything before it is fixed vocabulary and a predicate name, and the tool
parses it as it parses the rest of the command line. A malformed import --
an empty path included -- is therefore a usage error, status 2, as an
unknown `-F` format is:

```text
relic: error: invalid import: unknown column type 'symbl' in tsv:edge:symbl,symbol=edge.tsv
```

**The format is stated, never guessed from the path.** A pipe, `/dev/fd/3`
and a file named `data` are read the same way. A format added later is a new
word in the same place rather than a new option letter.

**Column types are words, not letters.** The mistake a declaration can still
make is to declare as a string a column the program joins as symbols, or the
reverse (section 4). A letter `s` would invite that mistake from either
side. The words are the ones the language's own documentation uses.

**`-` is standard input only here.** A file operand has no such spelling,
because no operands at all is how a program is read from standard input.
Section 8 says which reader gets standard input.

### 3.2 Through the library

```c
enum relic_format {
	RELIC_FORMAT_CSV = 1,
	RELIC_FORMAT_TSV
};

enum relic_column {
	RELIC_COLUMN_SYMBOL = 1,
	RELIC_COLUMN_STRING,
	RELIC_COLUMN_INTEGER,
	RELIC_COLUMN_SKIP
};

#define	RELIC_IMPORT_HEADER	0x1u

typedef struct relic_import relic_import_t;

struct relic_import {
	const char		*predicate;
	const enum relic_column	*columns;
	size_t			 column_count;
	enum relic_format	 format;
	unsigned int		 flags;
};

int	relic_import_file(relic_t *, const char *source_name, FILE *,
	    const relic_import_t *);
int	relic_import_path(relic_t *, const char *path,
	    const relic_import_t *);
```

The entry points name their input first, as every loader does, and then
what the input is. `relic_import_path()` opens the path, names diagnostics
by it and closes it. `relic_import_file()` reads a stream the caller owns,
under the name given, or `<input>` when that is NULL, as
`relic_load_file()` does. The description, and the column array it points
at, are borrowed for the call, and nothing of them is kept.

`relic_import_t` is a complete type: a caller fills it in field by field,
as it fills in a `relic_config_t`, and an accessor per field would buy
nothing. Its layout is part of the binary interface. `flags` is where a
later boolean goes without changing that layout. **The enumerators start at
one,** so a description or column array left zeroed -- by `calloc(3)`, or a
field forgotten -- is refused rather than read as csv and symbols.

The tool is a client of this interface. It parses an operand into a
`relic_import_t` and calls `relic_import_path()`, or `relic_import_file()`
over standard input for `-`. The words of section 3.1 are the tool's
spelling. The library has no spelling of its own, and takes the
enumerators.

An import is refused, with a diagnostic and nothing read or added, when:

* the description, or a path to open, is NULL, or a stream to read is NULL;
* `predicate` is NULL, does not match `[a-z][A-Za-z0-9_]*`, or is `not`, as
  `relic_fact_add()` refuses a name;
* `column_count` is zero, or `columns` is NULL;
* a column is not one of the four enumerators;
* more than 32 columns are kept, the structural arity limit of D-7;
* `format` is not one of the two enumerators;
* `flags` holds a bit the library does not know. A library ignores a
  configuration switch it predates, and can safely do so, because no switch
  changes an answer (architecture section 49, `specs/design-api.md`
  section 7). An import flag changes what is read: a library that ignored a
  header flag it did not know would import the header as a fact.

**An import needs no check after it.** It adds facts and nothing else, as
`relic_fact_add()` does, so a program that checked before an import still
checks after it, and an embedder may import between queries.

**Both entry points enter the context as a load does.** Caller code that
runs during an import finds the context busy -- the stream's read function,
which may be the caller's own (`specs/design-api.md` section 6), and the
diagnostic callback alike.

### 3.3 Why program text cannot name an import

No statement of a program names a file. Souffle's `.input` and Nemo's
`@import` allow one. relic does not, for three reasons:

* **Program text is untrusted.** relic is designed on the assumption that a
  program may come from someone else. The fuzz targets take arbitrary
  programs, the hash seed is random so that no program can be crafted
  against it, and the text and json output escape the control bytes a
  program's strings hold. Loading a program gives it no capability but
  computation. A directive that opened a path would let a program read any
  file the user can read and print it as rows, `/home/user/.ssh/id_ed25519`
  among them. With imports named only by the caller -- whoever runs the
  tool or embeds the library -- a program still opens nothing.
* **The program stays a function of its text.** Data named by the caller is
  an argument, as input is to every other Unix filter. A program that named
  its own files would mean something different on every machine it was
  loaded on.
* **The grammar stays as it is.** A directive would be the grammar's first
  statement about the environment rather than about relations, added for
  something the caller can already say.

The price is that a program does not carry its data's location. A command
line, or the script around it, carries it instead.

---

## 4. Column types

**Nothing about a field's type is inferred.** Each column's type is
declared, and a field that is not a value of its column's type is an error.

| Column | Accepts | Becomes |
| --- | --- | --- |
| `symbol` | the lexer's symbol rule, `[a-z][A-Za-z0-9_]*` | a symbol of those bytes |
| `string` | any bytes: none, NUL and bytes above 0x7f included | a string of those bytes |
| `integer` | the lexer's integer rule, `-?[0-9]+`, within `int64_t` | an integer |
| `_` | whatever the format can hold | nothing; the field is not part of the fact |

The three types are distinct domains (architecture section 10.1). The symbol
`ann` and the string `"ann"` are different values, and a join between them
fails without a word. The two ways of not declaring types would each let
that failure pass silently:

* **Guessing a field's type from its bytes.** Whether a join fails would
  then depend on the data. A column of names would import `ann` as a symbol
  and `Ann` as a string, and join on one and not the other.
* **Reading each field as a constant, spelled as a program spells it.**
  This needs data written in relic's own quoting, which ordinary CSV is
  not. relic's own csv output does not write it either: csv is untyped, and
  a symbol and a string with the same bytes are written alike
  (`specs/design-cli.md` section 4.2).

A declared type moves the decision to one place, made once per column, and
every field that does not fit it is reported: `Ann` in a symbol column fails
at its line.

**A field is typed after the format has decoded it.** csv's quotes and
tsv's escapes belong to the format, not to the value. So `"7"` in a csv
integer column is the integer 7, as RFC 4180 reads it, and `a\tb` in a tsv
string column is three bytes.

**Symbol and integer mean what they mean in a program.** The symbol rule is
the lexer's `relic_lex_is_symbol()`, which `relic_value_symbol()` shares.
The integer rule is one function over a byte range, which the lexer and the
importer both call. It accepts the range only when the whole range is
`-?[0-9]+` and the value fits `int64_t`, and it reads nothing past the
range. When it refuses, it says which way: the range is not an integer, or
it is one outside `int64_t`, since each caller reports the two differently.
The lexer finds a literal's extent before calling it; the importer passes a
whole field. So `7 `, `7x` and `1-` are not integers, and one
definition of each rule serves the language, the value constructors and
the import. `not` is a symbol value, as it is in `status(foo, not).` A
leading zero is accepted, as it is in a program: `007` is the integer 7,
and a column of codes whose zeros matter is a string column.

**No whitespace is trimmed.** ` 7` is not an integer and `a ` is not a
symbol, and in a string column both spaces are data. RFC 4180 counts spaces
as part of a field, and trimming them would change what a string column
holds.

**A value is interned as a program's constant is.** A symbol or string
field's bytes go into the symbol table and its value into the value table,
as the lexer and parser intern a constant. An integer goes into the value
table as the parser interns a literal. An imported `a` is therefore the
same value as the `a` of a rule, which is what lets them join.

**A skipped field is read, not decoded.** The format's rules hold for it --
its quoting, its escapes -- so a record's structure does not depend on which
columns are kept. It becomes no value and nothing of it is interned. `_` is
spelled as the language's anonymous variable because it does the same
thing. It exists because a table is often wider than the relation a program
wants. Importing every column and projecting with a rule would intern every
column's values, and a table wider than 32 columns could not be imported at
all.

---

## 5. The formats

Each format is read as the exact inverse of the tool's output format of the
same name (`specs/design-cli.md` sections 4.2 and 4.3). The output of one
query, written with `-F csv` or `-F tsv`, reads back under its columns'
types as the facts it came from, and section 11's round trip checks that.
The output of several queries is not one table: the blank line that
separates their results (`specs/design-cli.md` section 4.6) would be read
as a record. Each format also reads what other tools write in it, wherever
that does not conflict with being that inverse.

### 5.1 Records

In both formats:

* Outside a quoted csv field, a record ends at a line feed, or at a
  carriage return followed by a line feed. The carriage return of that pair
  belongs to the line end, so a file written on Windows reads as one written
  anywhere else. Inside a quoted csv field, both bytes are data (section
  5.3).
* The last record needs no line end. A file that ends in a newline has no
  empty record after it.
* An empty input holds no records. Importing it is legal and commits
  nothing.
* An empty line is a record of one empty field. It is the row of one empty
  string, and the row of no columns that section 3 reads with one `_`
  column. In an import of more than one column, or of one symbol or integer
  column, it is an error.
* Every record has exactly as many fields as the import has columns,
  skipped ones included.
* Bytes are data. No encoding is assumed, no byte-order mark is removed and
  no locale is consulted. A field holds exactly the bytes the format says it
  holds.

### 5.2 tsv

* Fields are separated by one tab.
* Inside a field a backslash begins an escape, and there are exactly four:
  `\\`, `\t`, `\n` and `\r`, the four the tsv output writes. A backslash
  before any other byte, or at the end of a field, is an error rather than
  a literal backslash. The lexer refuses unknown escapes for the same reason
  (`specs/design-parser.md` section 3.4): once `\q` imports as a backslash
  and a `q`, giving it any other meaning later changes what existing files
  import as.
* A carriage return that is not part of a line end is an error. The output
  never writes one; it writes `\r` instead. A raw one is either a file whose
  lines end in a carriage return alone, or a byte a producer failed to
  escape, and neither should become data silently.
* Every other byte, NUL included, is data.

A tool that does not escape its backslashes writes files this reader
refuses: a Windows path in a field fails at its first backslash, with its
line and column. Doubling the backslashes turns such a file into relic's
tsv. PostgreSQL's text `COPY` format uses the same backslash convention with
more escapes -- `\b`, `\f`, `\v` and `\N` among them -- which this reader
refuses, so such an export imports only when it holds none of them. The
refusal is the price of reading relic's own output back exactly. A reader
that took backslashes literally could not recover a tab or a newline inside
a string, which relic writes as escapes.

### 5.3 csv

RFC 4180, as the output writes it:

* Fields are separated by commas.
* A field that begins with `"` is quoted. It ends at the next `"` that is
  not the first of a doubled `""`. Inside it, `""` is one `"`, and commas,
  carriage returns and line feeds are data, so a carriage return followed
  by a line feed is two bytes of the field, not a line end. After the
  closing quote the field must end: the next byte is a comma, a line end or
  the end of the input, and anything else is an error. A quoted field still
  open at the end of the input is an error, reported at its opening quote,
  as an unterminated string literal is.
* A field that does not begin with `"` is unquoted, and ends at the next
  comma or line end. A `"` inside it is an error. RFC 4180 requires a field
  that holds a quote to be quoted, and a reader that let one through would
  have to guess what a producer meant by `a"b,c"`.
* A carriage return in an unquoted field that is not part of a line end is
  an error, for the reason section 5.2 gives.
* `""` and an empty unquoted field are both the empty string. Some tools
  draw a distinction between the two, but relic has no null for the
  distinction to map to.

### 5.4 The header

With `header`, the first record is a header and is not imported. It is read
by the format's rules and must have as many fields as the import has
columns: a header of the wrong width is the cheapest sign that the
declaration is wrong, and checking it costs nothing. Its fields are not
typed, decoded or interned. An import with `header` of an input that holds
no record is an error, because the flag says a header is there.

Without `header`, the first record is data. A header over an integer column
then fails at that field, loudly. Over symbol and string columns, a header
such as `id,name` would import as a fact, so a table with a header needs
the flag. The flag is not a guess. Detecting a header would mean inferring
something about a table from its data, which section 4 refuses to do.

---

## 6. Reading and committing

### 6.1 One pass over the bytes

An import reads its stream a buffer at a time and holds none of its text.
What it keeps of a record is the staged tuple, and once a field has been
decoded, its text is of no further use.

**The reader is one state machine, which alone decides where fields and
records end.** Its state says where it stands. Each state also says what
the end of the input means there, since the end is not a byte:

| State | The end of the input |
| --- | --- |
| at the start of a record | ends the table; there is no record |
| at the start of a field, after a separator | ends an empty last field, and the record |
| inside an unquoted field | ends the field and the record |
| inside a quoted csv field | is an unterminated quoted field |
| just after a quote inside a quoted field -- the first of a doubled `""`, or the closing quote | the quote closed the field; ends it and the record |
| just after a tsv backslash | is a backslash at the end of a field |
| just after a carriage return, where only a line feed may follow | is a carriage return inside a field |

Each byte moves the reader from one state to the next, and every boundary
follows from the state the byte arrives in. A line feed outside a quoted
field ends the record; at the start of a record it is an empty line, a
record of one empty field. Inside a quoted field a line feed is data.
Nothing else, such as a second pass or a count of quotes, decides whether a
field is open. A misplaced byte is an error in the state it arrives in, so
a stray quote or a lone carriage return is reported when it is read, not
after the lines that follow it have been gathered.

**The reader emits events, and a consumer acts on them.** The reader emits
three events: a field has ended, with its decoded bytes; a record has
ended; and a mistake, at its location. It is a function of its state and
the next bytes, with no input or output and no context, so it reads a table
the same way whether the table arrives in one buffer or a byte at a time,
and the unit tests drive it directly. The consumer holds the tuple being
built. It types each field as it ends -- interning it, or dropping it when
its column is skipped -- and stages the tuple when the record ends. The loop
that reads the stream feeds the reader and hands its events to the consumer
in order, so a mistake the consumer finds in a field is met before anything
the reader finds after that field (section 6.3).

**It holds one field and one tuple.** A field's decoded bytes accumulate in
one buffer, which grows by doubling to the longest field and is reused.
Neither a record's text nor the file's is ever held.

### 6.2 Staging and the commit

An import is a load whose text is a table. It stages its facts as it reads
them. When the whole input has been read, it commits them through the
commit of `specs/design-facts.md` section 4.3, which that document's
section 13 describes as "the same commit fed by a different reader". It
follows that:

* **An import is all or nothing.** A read error, a malformed record, a
  field its column refuses, or a limit that refuses the commit adds no fact,
  and the base stores stay as they were.
* **A limit refuses at the commit, with a load's message,**
  `limit exceeded: tuples=N while loading edge/2`, before anything has
  grown.
* **Duplicates are held once.** This holds within an import and against
  the facts a base store already has. An import that adds nothing new
  leaves the relations current (`specs/design-facts.md` section 5.4).
* **Staging is charged to no limit,** as a load's is. It is proportional to
  the input the caller chose (section 9).
* **What a failed import interned stays interned.** The predicate is
  interned when the import begins reading, as a fact's predicate is when the
  parser stages it, and values as their fields are read. A failed import
  leaves them interned, holding nothing and referenced by nothing, as a
  failed load does (`specs/design-parser.md` section 6).

An import that commits nothing defines nothing. A predicate is defined when
its base store holds tuples (`specs/design-facts.md` section 3.5), so a
program that reads a predicate imported from an empty table draws the
undefined-predicate warning. That warning is accurate: the data held no
rows.

### 6.3 The first mistake ends an import

The parser reports every broken statement, because each is a separate
mistake the author made. A table is usually written by a program, and its
mistakes are systematic: a wrong delimiter, columns declared in the wrong
order, a header without the flag. Such a mistake breaks every record, and a
million identical messages help no one. An import therefore stops at its
first mistake and reports only that one. The import fails either way, and
the first bad field is the one to look at. The tool still attempts every
import and every file, so one run reports the first mistake in each of
them.

**The first mistake is the first the reader meets reading forward.** A
field's bytes are checked as they are read. Its type is checked when it
ends. A record's width is checked when a field beyond the last column
begins, or when the record ends short. With one pass and one state
(section 6.1), the mistake reported is determined by the input alone.

### 6.4 Order

Records are staged in input order. Values are therefore interned, and each
base store's tuples appended, in the order the table holds them, as a
load's are (`specs/design-facts.md` section 4.5). The predicate is interned
before any value of the import.

---

## 7. Diagnostics

Every mistake in a table is reported at a location: the source name (the
path, or `<stdin>` for `-`) and the line and column of the byte the reader
should look at. Both count from one, and columns count bytes, as the
lexer's do (`specs/design-parser.md` section 4). A line is a physical line,
so the line feeds inside a quoted csv field advance it. Lines and columns
are counted in the `uint32_t` a location holds, and stop at its largest
value rather than wrapping, so a table of more lines than that reports its
later mistakes at the last line a location can name, never at an earlier
one.

| Mistake | Located at | Message |
| --- | --- | --- |
| a field its column refuses | the field's first byte | `expected a symbol, found "Ann"`; `expected an integer, found an empty field`; `expected a symbol, found a field holding 0x09` |
| an integer out of range | the field's first byte | `integer out of range: "9223372036854775808"` |
| too many fields | the first byte of the first extra field | `expected 2 fields, found more than 2` |
| too few fields | the end of the record | `expected 2 fields, found 1` |
| an unknown tsv escape | the byte after the backslash | `unknown escape: a backslash followed by 'q'` |
| a tsv backslash ending the input | the end of the input | `unknown escape: a backslash at the end of the input` |
| a stray carriage return | the carriage return | `carriage return inside a field` |
| a quote in an unquoted csv field | the quote | `quote inside an unquoted field` |
| a byte after a closing quote | that byte | `expected ',' or the end of the line after a closing quote, found 'x'` |
| an unterminated quoted field | its opening quote | `unterminated quoted field` |
| a missing header | the end of the input | `expected a header, found the end of the input` |

```text
people.csv:2:1: error: expected a symbol, found "U2"
people.csv:3:9: error: expected an integer, found "x"
```

A record with too many fields is reported when its first extra field
begins, and the message does not count the rest. Counting them would mean
reading on past the first mistake, through bytes that may hold mistakes of
their own -- a quote opened in the fourth field can run to the end of the
input -- so the count would depend on what follows the mistake reported,
which section 6.3 rules out.

A failure to read has no location and names the system's reason,
`cannot read people.csv: Input/output error`, as a load's does.

**A message is printable ASCII with no backslash.** The message goes to
whatever diagnostic callback is installed, and an embedder's callback may
show it without escaping it, so the bytes of a table must not reach the
callback raw. The tool's formatter escapes every message again, and doubles
each backslash so that its own escapes cannot be mistaken for the text's
(`lib/diag.c`). A message that held a backslash would therefore misstate
the table on the tool's standard error. So:

* **A field is quoted, in double quotes, when all its bytes are printable
  ASCII other than a double quote or a backslash.** Past forty bytes it is
  cut, and `...` follows the closing quote, as the parser cuts the text it
  quotes (`specs/design-parser.md` section 7.2). Nearly every mistake in
  practice -- `Ann`, `x`, `7.5` -- is quoted in full.
* **Any other field is named by its first byte that is not,** as `a field
  holding` followed by that byte. The location still points at the field,
  and the byte tells the reader what to look for in it.
* **A single byte is quoted as the parser quotes one:** as a character in
  single quotes when it is printable and not a backslash, and otherwise as
  `0x%02x`.

The field quoted is the decoded one, the value it would have been, so a
csv field's surrounding quotes are not part of it.

---

## 8. The tool

**Imports load in the load phase** (`specs/design-cli.md` section 2, phase
2), after the program: first the program's source, then each `-I` in
command-line order. The program comes first so that a run's diagnostics
come in the order a reader looks for them, the program's and then the
data's. The order changes no answer: loads and imports append to one
program, and nothing is evaluated until every one has returned. Every
import is attempted even after a file or an import has failed, as every
file is.

**`-c` checks the data too.** Every import is read, split and typed in
phase 2, so a table that does not match its declaration is reported without
anything being evaluated.

**Standard input has one reader.** The session under `-i` and each import
of `-` claim it, through one claim made while the command line is checked,
and a second claim is a usage error naming both. When either has claimed
it, the program is the file operands, and with none it is empty, as under
`-i` (`specs/design-cli.md` section 2.1). Otherwise that section's rule
stands: with no file operands, the program is read from standard input.
The claim is on the spelling `-`. A path that names standard input some
other way, such as `/dev/stdin`, is a file like any other, read as the
caller asked.

**A mistake about standard input is loud.** A program piped to relic is
read as it always is, whether or not `-I` is given, so no program is ever
dropped silently. The opposite mistake -- tables named by path and no
program file, in a script whose standard input is meant for something
else -- reads that input as the program, and it is refused with the
parser's diagnostics rather than ignored. At a terminal it is the usage
error for a program nobody is typing. A table is queried with no program
file by importing it from standard input, or by naming `/dev/null` as the
program:

```sh
relic -I tsv:edge:symbol,symbol=- -q 'edge(a, X)?' < edge.tsv
relic -I tsv:edge:symbol,symbol=edge.tsv -q 'edge(a, X)?' /dev/null
```

**An import of `-` when standard input is a terminal is a usage error,** as
reading a program from one is: the tool would otherwise wait silently for a
table nobody is typing.

**Exit status.** A malformed `-I` operand, or a second claim on standard
input, is a usage error, status 2. An import that cannot be read, is
malformed or is refused by a limit is status 1, as a load that fails is.

Nothing else in the tool changes. `-e` counts imported facts in a
component's `facts: N` as it counts any fact (`specs/design-facts.md`
section 7). Output formats, `-s`, `-n` and `-v` act on queries, which do not
know where their facts came from.

---

## 9. Limits and untrusted input

A table is input the caller chose, as a load's text is, and it is treated
the same way.

* **The structural arity limit holds.** At most 32 columns are kept.
  Nothing else about the shape of a table is bounded.
* **The tuple and memory limits bound what an import commits,** at the
  commit (section 6.2), with nothing grown when they refuse it. They do not
  bound what an import holds before the commit. The import stages and
  interns the whole table before the commit refuses it: a table of a
  billion distinct rows under `-l tuples=1000000` is read to its end first.
  The values it interned stay interned for the life of the context, which
  an embedder feeding one long-lived context tables from an untrusted party
  must allow for. A load has the same property. An earlier refusal, as
  soon as staging holds more new tuples or bytes than the limits admit,
  belongs to the staging both share, and is an open item in `TODO.md`.
* **Nothing bounds the field buffer but the longest field.** A quoted csv
  field left open early in a file holds the rest of the file before it is
  reported, since a quoted field may hold line feeds. That is the exposure a
  load has when it reads a stream whole (`TODO.md`, "A stream is read whole
  with no size limit"), and it has the same answer. A caller that can bound
  its input does so before handing it over. A caller that cannot needs a
  separate limit, not a wider meaning for `memory`. Such a limit belongs
  with the other limits of a context in `relic_config_t`, not in an
  import's description. That structure is frozen, so adding the field is
  an incompatible change to the interface (`specs/design-api.md` sections
  9 and 11).
* **Crafted collisions gain nothing.** The staging sets, the symbol table
  and the value table all hash with the context's seed, so a crafted table
  can no more drive them into one probe cluster than a crafted program can.
* **A string column holds any bytes.** The text and json output formats
  escape them when a query prints them, and csv and tsv write them as they
  are (`specs/design-cli.md` section 4), as for any string.
* **Only the caller opens files.** No path is opened but the one the caller
  named (section 3.3).

---

## 10. Cost

The library gains a reader for the two formats, the field decoding, and the
two entry points with their validation. All of it lives in `lib/import.c`
and shares only the symbol and integer rules with the lexer. The tool gains
the operand parser and the claim on standard input. The estimate is about
350 non-comment lines in the library and 80 in the tool.

**The gate is correctness and time.** The round trip of section 11 holds,
and importing `facts-join`'s `row.tsv` at scale 4 takes no longer than
loading the same facts as text. The import's resident peak is measured and
recorded rather than gated: the expected saving, 24 bytes per fact, is
smaller than the spread between runs of one load, which is 165 to 199 bytes
per fact over ten runs. The peak is taken as the median of ten runs of
each, measured as `specs/design-facts.md` section 9 measures a load.

---

## 11. The tests

* **The reader.** `tests/unit/test_import.c` drives each format's state
  machine over byte strings, with no context, checking the events it emits
  and every error with its line and column:
  * every tsv escape, and every refused one;
  * quoted csv fields holding commas, doubled quotes, carriage returns and
    line feeds, including a carriage return followed by a line feed, which
    stays two bytes of the field, and a field that spans lines;
  * a quote in an unquoted field, a byte after a closing quote, and a quoted
    field left open at the end of the input;
  * line feed and carriage-return-line-feed line ends, a stray carriage
    return, and a last record with no line end;
  * the end of the input in each state of section 6.1: an empty input, a
    trailing line feed, an empty last field, and an empty line;
  * NUL and bytes above 0x7f in a string field;
  * records one field short and one field long, and a record with two
    mistakes, of which the first met is reported.

  Every table is fed once whole and once a byte at a time, and must read
  the same both ways.
* **Column types**, tested at their boundaries:
  * symbols: `a`, `aB_9` and `not` are accepted; `A`, `_a`, `1a` and an
    empty field are refused;
  * integers: `INT64_MIN` and `INT64_MAX` are accepted, and one past each is
    refused; so are `-`, `+1`, ` 7`, `7 `, `7x` and `1-`, while `-0` and
    `007` are accepted; a quoted `"7"` in csv is 7;
  * an empty string field is accepted;
  * a skipped column interns nothing: the value count is unchanged.
* **The library**:
  * every refusal of section 3.2, each with nothing read or added,
    including a zeroed description and a zeroed column;
  * all or nothing: a malformed last record adds none of the good ones
    before it, and a limit refusal grows nothing;
  * duplicates are held once, within an import and against existing facts;
  * an import after materialization discards what was derived, and the
    next query sees the new facts, under the default strategy and through
    the Magic rewrite;
  * with the header flag: a matching header, one of the wrong width, and an
    empty input;
  * a stream whose read function calls back into the context is refused, as
    a load's is.
* **The tool.** Golden tests in `tests/lang` cover:
  * each usage error of an `-I` operand: unknown format, unknown column
    type, missing `=`, empty path, bad predicate, `not`, 33 kept columns;
  * `-c` rejecting a table, and imports loading after file operands;
  * every diagnostic of section 7 with its location, including a field
    named by a byte it holds.

  The tables sit beside the tests, which run from their own directory as
  every language test does, and are distributed with them. The cases the
  golden harness cannot express, because it always passes a program operand
  and a standard input of its own, are in `tests/cli.sh`:
  * a query over an import of `-` with no program file, which is empty;
  * a program piped to standard input with `-I` naming a path, which is
    read;
  * `-` named twice, and `-` under `-i`.

  An import of `-` from a terminal is not tested, as the session's prompt is
  not: no portable test drives a terminal.
* **The round trip.** A program's facts hold every byte value in strings,
  in the positions each format treats specially: first, last, alone and
  doubled, and the pair of a carriage return and a line feed. They also
  hold symbols, integers at both ends of the range, and a row of no columns.
  They are written with `-F csv` and `-F tsv`, imported back with the
  columns declared, and queried again. The imported facts must equal the
  program's, compared in the typed text format, and writing them again must
  reproduce the first output byte for byte.
* **The fuzz target `import`.**
  * The input's first byte chooses a format, the header flag and a column
    list from a small fixed table: all strings; a symbol and an integer; a
    skip between two strings; one column; 32 columns. The rest of the input
    is the table, empty included.
  * The table is handed over as a stream with a read function of the
    target's own, built as the re-entry test builds one (`funopen(3)` or
    `fopencookie(3)`).
  * The target checks for no crash, no hang and no invalid access, as the
    load target does. As the eval target counts errors, it also checks that
    an import that failed reported at least one.
  * Its corpus is `tests/fuzz/corpus-import/`. Every `make check` replays it
    through this target alone, since the shared corpus of the other two is
    program text.
* **The gate** of section 10.

---

## 12. Order of work

1. **One integer conversion.** The lexer's conversion becomes the function
   of section 4, over a byte range, and the lexer calls it. The language
   goldens do not change.
2. **The reader.** The two formats' state machines and the field decoding,
   with the unit tests of the reader and the column types.
3. **The library.** `relic_import_t`, the two entry points, validation,
   staging and the commit, and the diagnostics, with:
   * the library tests;
   * `man/relic.3`;
   * the symbol audit;
   * the version-info bump for an added interface (`specs/design-api.md`
     section 9).
4. **The tool.** `-I`, the reading of standard input, and the load phase,
   with `man/relic.1`, the golden tests and `tests/cli.sh`.
5. **The fuzz target and the round trip.**
6. **The gate.** `facts-join` measured, and `bench/compare` changed so that
   relic reads each data set's TSV through `-I`, as the engines that read
   TSV do.

Each step ends with `make check` green. Steps 3 and 4 are separate because
the tool is a client of the library and gains nothing the library does not
offer.

---

## 13. Determinism

* The tool runs imports after the program, in command-line order.
* An import interns its predicate first, then its values in the order of
  their first appearance in the table.
* Each base store's new tuples are appended in the order the table holds
  them.
* The mistake an import reports is the first the reader meets (section
  6.3).

So two runs over the same program and tables produce the same identifiers,
plans, rows and diagnostics, in the same order.

---

## 14. Amendments this design makes

* Architecture:
  * section 40: `-I import` in the usage and the option list;
  * section 41: an import example;
  * section 48: `relic_import_t` joins the complete types;
  * section 50: the import entry points beside the loaders;
  * section 64: `lib/import.c`;
  * section 67: an import fuzz target beside the two over program text;
  * section 69: CSV and TSV import points here;
  * section 76: logs `-I`, the new complete type and the new fuzz target.
* `specs/design-cli.md`:
  * section 2: imports are part of the load phase, after the program;
  * section 2.1: an import of `-` claims standard input as the session
    does, a second claim is a usage error, and a claimed standard input
    leaves the program the file operands or empty;
  * section 2.3: the new usage errors.
* `specs/design-api.md`: the import entry points and types; the unknown
  flag refused, where a configuration switch is ignored; an import needing
  no check, as `relic_fact_add()` needs none.
* `specs/design-parser.md` section 3.5: the integer conversion is the
  function of section 4, shared with import, reading nothing past its range.
  Section 3.6's contract, whose only reason is `strtoll(3)`, then has none
  and is removed.
* `specs/design-facts.md` section 13: the CSV and TSV item points here.
* `specs/design-hardening.md` section 5: the import target and its corpus.
* `specs/design-bench.md`: relic reads the data sets' TSV through `-I`.
* `include/relic.h`, `man/relic.1` and `man/relic.3`.
* `STATUS.md` and `TODO.md` follow the implementation.

---

## 15. What is deliberately absent

* **Type inference** (section 4).
* **An import directive in program text** (section 3.3).
* **Other delimiters and quote characters.** There are two formats, each
  the fixed inverse of an output format. A table separated by semicolons,
  as some spreadsheet locales write, is converted by its producer. If one
  is needed, it is a new format word in the slot section 3.1 already has.
* **Columns named by the header.** A header is skipped, not read. relic's
  columns are positional, as its output's are (`specs/design-cli.md`
  section 11), and choosing columns by name would be a second way to say
  what `_` says.
* **Nulls.** relic has none. An empty field is the empty string, or an
  error in a symbol or integer column.
* **Trimming, comments, encodings and byte-order marks.** Bytes are data
  (section 5.1). The byte-order mark some spreadsheet exports write is part
  of the first field:
  * in a header whose first field is unquoted, it is skipped with the
    header;
  * in one whose first field is quoted, it makes the field unquoted with a
    quote inside it, and the header is refused;
  * without `header`, it is an error in a symbol or integer column, and data
    in a string column.
* **Continuing past the first mistake** (section 6.3).
* **Catching a column declared string that the program reads as
  symbols.** Every symbol's bytes are also a valid string, so the table
  cannot reveal the mistake, and only the declaration can prevent it. That
  is why types are spelled as words.
* **Committing in batches.** An import commits once, at its end, as all or
  nothing requires. Batches would bound staging, but a failed import would
  leave part of a table behind, and each batch would discard what had been
  derived.
* **Importing from the interactive session.** A session's lines may come
  from a script as easily as from a person. A command that named a file
  would let those lines open one, which section 3.3 keeps from program text
  for the same reason.
* **An import from bytes in memory.** A caller that holds a table hands it
  over as a stream of its own. `fmemopen(3)` serves a table that is not
  empty; it refuses a buffer of no bytes on some systems, and `funopen(3)`
  or `fopencookie(3)` serves any table.
* **Writing tables.** Writing a relation as a table is architecture section
  69's relation export. A query written with `-F csv` or `-F tsv` writes
  one, and this design reads exactly that back.
