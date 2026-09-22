# relic parser design

**Status.** Design document for the front end: the byte-oriented lexer, the
recursive-descent parser, the abstract syntax tree they produce and the
diagnostics they emit. Forward-looking, like every document under `specs/`;
`STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 5 through 13, 17, 18, 50, 56
and 67; `specs/design-memory.md` for the arenas and the interning tables;
`relic-plan.md` decisions D-1, D-2, D-4, D-5, D-6, D-12, D-15 and D-16.

---

## 1. What this document settles

* what the bytes of a source file mean, without reference to a locale;
* what the parser produces, and where it lives;
* which values are interned during parsing and which are not;
* what happens after the first mistake in a file, so that the second one is
  still reported;
* what a load does to the program when it fails.

It does not cover predicate identity, variable numbering or safety. Those
turn an abstract syntax tree into a program and belong with the program model.

---

## 2. Shape of the front end

```text
bytes -> relic_lexer_t -> relic_token_t -> relic_parser_t -> relic_ast_program_t
```

Two modules with one direction of flow and no shared mutable state beyond the
context they both intern into.

**`lib/lex.c` decides what a byte sequence is.** It classifies bytes from its
own table, recognizes the token, decodes a string literal's escapes, converts
an integer literal and interns the bytes of every name and string literal. It
emits no diagnostics: a token that is not well formed comes back as
`RELIC_TOKEN_ERROR` carrying the reason, and the parser decides whether to
report it.

**`lib/parse.c` decides what a token sequence means.** It builds the tree,
owns every diagnostic the front end produces, and owns error recovery.

The split is what keeps error recovery in one place. A lexer that reported for
itself would have to be told when the parser is skipping wreckage and wants
silence, which is a flag threaded between two modules to make one of them stop
talking. Returning the reason with the token removes the flag.

---

## 3. Lexical structure

### 3.1 Byte classification

Classification comes from a 256-entry table in `lib/lex.c` and never from
`<ctype.h>`. `isalpha()` answers a question about the process locale, and the
answer decides whether a program parses; relic's answer must be the same in
every locale, on every platform, in both directions of `char`'s signedness.

The table records what a byte may do rather than what it is, because several
of those are true at once: `a` begins a symbol, continues an identifier and is
a hexadecimal digit. Six properties cover the language -- separates tokens,
is a decimal digit, is a hexadecimal digit, begins a symbol, begins a
variable, continues an identifier -- and `_` carries the last two, so that
identifier scanning needs no special case for it.

A byte with none of them is part of no token: that is every byte above 0x7f,
every control byte, and the punctuation, which is recognized by value because
each mark means one thing. Outside a string literal, such a byte is an error;
inside one it is data and is not classified at all.

### 3.2 Tokens

```text
symbol          a lowercase-initial identifier: [a-z][A-Za-z0-9_]*
variable        an uppercase- or underscore-initial identifier:
                [A-Z_][A-Za-z0-9_]*
string          "..." with escapes, holding arbitrary bytes
integer         -?[0-9]+, converted to int64_t
compare         = != < <= > >=, one token kind carrying which
implies         :-
( ) , . ?       punctuation
end of input
error           a byte sequence that is not a token
```

Identifiers are ASCII, so `$` is not an identifier byte and a generated name
containing one cannot collide with a name a user could write.

Comparison operators are one token kind with an operator field rather than six
kinds. The parser never wants to know which operator it has until it stores
it, and a single kind keeps the token switch from listing six cases that do
the same thing.

`not` is an ordinary symbol token. It is recognized as the negation keyword by
the parser, and only where a literal may begin, so `status(foo, not).` stores
the symbol `not` as a value. The alternative, a reserved word, would reject
data a user could reasonably have.

What the rule costs is the name: no predicate may be called `not`, and the
parser says so in both of the positions where one could be written -- at the
start of a literal, where the word is the keyword, and after the keyword,
where `not not` would otherwise name a relation. Rejecting the second is not
tidiness. A predicate of that name can never be *defined*, because a fact
beginning `not` is read as a negation, so a reference to one is a typo that
would otherwise reach every later stage looking like an ordinary predicate
that simply has no rules.

### 3.3 Comments and whitespace

`%` begins a comment that ends at the next newline or at the end of input; a
comment that is not newline-terminated is not an error, because a file whose
last line is a comment is ordinary. There are no block comments, so there is
no such thing as an unterminated comment.

Space, tab, newline, carriage return, vertical tab and form feed separate
tokens and are otherwise ignored.

### 3.4 String literals

A string literal holds arbitrary bytes. Bytes above 0x7f pass through
unexamined, so UTF-8 survives without the lexer knowing what UTF-8 is, and a
NUL byte written as an escape is part of the string: the symbol pool records a
length beside its bytes precisely so that it can be.

Six escapes plus one numeric form are recognized:

```text
\\  \"  \n  \t  \r  \0  \xHH
```

`\xHH` takes exactly two hexadecimal digits in either case. An escape that is
not in this list is an error rather than a silent literal character. The
permissive reading cannot be taken back: once `"\q"` means `q`, defining `\q`
later changes the meaning of a program that already parses.

**A raw newline does not appear inside a string literal.** A literal that
reaches the end of a line is reported as unterminated, at its opening quote.
Allowing the newline would be defensible in isolation, but a missing closing
quote is one of the two most common typos in a quoted language, and allowing
it means one missing quote swallows the rest of the file and reports its
failure hundreds of lines from the mistake. A newline inside a string is
written `\n`.

### 3.5 Integer literals

Converted with `strtoll(3)` into `int64_t`, base ten. A value outside the
range is an error that names the literal, so that `-9223372036854775808` is
representable and `9223372036854775808` is diagnosed rather than wrapped.

A `-` immediately preceding a digit is part of the literal token. This is
unambiguous because v1 has no arithmetic, so a `-` cannot be an operator.
A `-` anywhere else is not part of any token.

An identifier byte immediately following the digits is an error rather than
the start of a new token. Without the check, `0x10` would lex as the integer
`0` followed by the symbol `x10`, and the complaint would be about a missing
comma several bytes later. This is the one place the lexer looks past the
token it has just recognized, and it does so to make a misspelled literal
report itself.

### 3.6 The input contract

The lexer scans a byte range and requires one byte past its end to hold a NUL.
`strtoll(3)` and nothing else needs it: the terminator is what stops the
conversion from reading past a digit span that ends at the end of the buffer.
Every loading entry point allocates the text with room for it, so no caller
has to know.

A NUL *inside* the range is an ordinary byte and is not a token, so it is
reported as an unexpected character.

---

## 4. Locations

Every token carries a `relic_location_t`: the source name, and the line and
column of the token's first byte, both counting from one.

Columns count bytes rather than characters or display cells. A tab is one
column. Anything else would require the lexer to know an encoding and a tab
stop, and the number is there to let a reader find the byte, not to align a
caret under it.

The source name is interned in the symbol pool and the location holds the
pointer the pool returns. Those bytes live in the context arena and never
move, so a location stored in the tree stays printable for the life of the
context, and loading the same file twice stores one copy of its name.

---

## 5. The tree

`lib/ast.h` defines five types. They are a transcription of the grammar of
architecture section 13 and deliberately nothing more: no predicate table, no
variable numbering, no reordering. What the parser accepts is a syntactic
question, and what it produces should still be recognizable as the text.

```c
term      := a constant's domain identifier, or a variable
atom      := predicate name, arity, terms
literal   := a positive atom, a negated atom, or a comparison
statement := a rule (head plus a possibly empty body), or a query (a body)
program   := a list of statements
```

**A fact is a rule with an empty body.** There is no third statement kind and
no `is_fact` flag, because either would be a second way to say something the
body length already says, and two representations of one fact eventually
disagree. `edge(a, b).` is a rule whose body has no literals, which is also
exactly what it means.

**A term is a domain identifier or a variable.** A constant is interned as it
is parsed, so the tree holds the same 32-bit identifier a tuple column will
hold and no later stage re-derives it. A variable holds the interned symbol of
its name, for diagnostics and for the numbering the program model will do.

**Anonymous variables are marked, not recognized by name.** Every occurrence
of `_` is a distinct variable, while `_Unused` is an ordinary named one, so
the distinction is a flag on the term rather than a string comparison repeated
wherever variables are examined.

**Statements form a `TAILQ` rather than an array.** They are allocated from
the program arena, which cannot resize a block, and a growable array would
leave every superseded generation of itself in the arena. The terms of an atom
and the literals of a body *are* arrays, because their lengths are known when
they are copied into the arena and the planner will index them.

---

## 6. What the parser interns, and when

Interning during parsing rather than after it is what keeps the tree small and
uniform, and architecture section 18 requires it: the evaluator compares
identifiers, and something has to produce them.

| Text | Interned as | By |
| --- | --- | --- |
| a predicate name | symbol identifier | the lexer |
| a variable name | symbol identifier | the lexer |
| a string literal's decoded bytes | symbol identifier | the lexer |
| a symbol used as a term | value, `RELIC_SYMBOL` | the parser |
| a string used as a term | value, `RELIC_STRING` | the parser |
| an integer literal | value, `RELIC_INTEGER` | the parser |

The lexer interns *bytes* and the parser interns *values*, and the division is
not arbitrary. An identifier token does not yet know whether it names a
predicate or is a symbol constant, and interning it as a value on sight would
give every predicate name a domain identifier it never uses. The lexer
therefore produces the symbol identifier, which both uses need, and the parser
produces the value only where a term is being built.

Interning is permanent and context-lifetime. A file that fails to parse leaves
its symbols and values interned, which costs a little memory and nothing else:
identifiers are assigned in first-appearance order, and first appearance
includes an appearance in a statement that was later discarded.

---

## 7. Diagnostics and error recovery

### 7.1 One message per statement

Error recovery synchronizes on the statement terminators, `.` and `?`. After
reporting, the parser discards tokens until it has consumed one of them, or
reaches the end of input, and resumes there.

**Between the report and the resumption, the parser stays silent.** The first
mistake in a statement is the one worth reading; everything after it is the
parser's confusion, not the writer's. The rule is therefore that a statement
produces at most one diagnostic, which makes the count of messages the count
of broken statements, and makes a file with five mistakes report five lines.

Lexical errors obey the same rule, because they reach the same reporting
function. A line of binary in the middle of a program is one message, not one
message per byte.

### 7.2 What a message says

Messages follow architecture section 30: lowercase, one line, located at the
byte the reader should look at. A message that names what was expected also
names what was found, because "expected ','" alone leaves the reader to
discover that the problem is a missing closing parenthesis two lines up.

```text
policy.dl:12:18: error: expected ',' or ')', found '.'
policy.dl:19:1: error: unterminated string literal
policy.dl:24:9: error: integer literal out of range: 9223372036854775808
```

A byte quoted in a message is quoted as a character when it is printable and
as `0x%02x` when it is not. The stderr formatter escapes what it prints in any
case, so this is about readability rather than safety.

Text quoted from the source is truncated past forty bytes. A message names one
thing and the reader needs enough of it to recognize which, not all of it; the
only message that quotes source text is the out-of-range integer literal, and
a literal long enough to be cut is one whose first forty digits have already
made the point.

### 7.3 Counting and returning

The parser counts statements it reported on. A load with a nonzero count fails
even though every other statement parsed, because a program that is missing a
rule is not a smaller program, it is a different one.

**A parse function returns failure only after the load has been made certain
to fail.** There are three ways that happens -- a diagnostic was reported and
counted, an unrecoverable failure was flagged and parsing stopped, or the
program arena was marked -- and the point of naming them together is that
there is no fourth. The one that could have been forgotten at a call site,
allocation, is funnelled through a single function that marks the arena on
every refusal, including the ones it decides itself rather than passing to the
arena. A function that returned failure having done none of the three would
drop a statement and report success, which is the one outcome worse than an
error.

---

## 8. Loading

```c
int relic_load_string(relic_t *, const char *source_name, const char *text);
int relic_load_file(relic_t *, FILE *, const char *source_name);
int relic_load_path(relic_t *, const char *path);
```

`relic_load_path()` opens the file and delegates; `relic_load_file()` reads the
stream into one buffer and delegates. A stream is read whole rather than
incrementally because the lexer needs to look one byte past a token, the parser
needs no more input than that, and an incremental reader would buy a bounded
footprint that a program's abstract syntax tree does not have anyway.

Multiple loads append to one logical program, which is what lets facts and
rules be split across files on a command line.

**A load is all or nothing.** Statements are parsed onto a list of the load's
own and spliced into the program only if the whole input parsed. A caller that
ignores the return value is then working with a program that never contains
half of a file, and `relic -c` can report every mistake in a file without the
next stage having to wonder which statements survived.

What a failed load does keep is its arena memory: the discarded statements were
allocated from the program arena and are not individually reclaimable. A
program that loads a broken file repeatedly grows slowly. This is worth stating
rather than hiding, and it is bounded by the size of the text the caller chose
to load.

**Exhaustion of the program arena is caught at the load boundary.** Parsing
checks its allocations as ordinary C does, and the sticky failure flag lets a
half-built statement stop without an error path threaded through every level.
The load then asks the arena whether anything failed before it splices, so a
missed check cannot become a truncated program. The flag is cleared there, once
the failure has been reported: the load is the phase the flag belongs to, and
leaving it set would make one transient exhaustion refuse every later load.

---

## 9. Limits

The arity limit of architecture section 63, at most 32 arguments, is
structural and not a parser convenience: an index key is a `uint32_t` column
bitmask, so a 33rd column could not be named by one. The parser is merely
where it is cheapest to enforce, because rejecting the 33rd argument where it
was written says more than rejecting the predicate later, and because the
bound lets an atom's terms be collected in a fixed buffer with no allocation
and no failure path. The variable-count limit belongs to variable numbering
and is enforced with it.

Nothing else is bounded. A body may hold any number of literals and a file any
number of statements; both are bounded by the text the caller supplied.

---

## 10. Determinism

The front end contributes three things to the reproducibility of a run, and
all three are properties of this document rather than of the code that
implements it:

* **classification is not locale-dependent**, so the same bytes are the same
  tokens everywhere;
* **identifiers are assigned in order of first appearance**, and the parser
  visits the text in order, so the same file always produces the same
  identifiers;
* **diagnostics are derived from locations and interned text**, never from a
  pointer value or an iteration over a hash table.

---

## 11. What is deliberately absent

* **A generated lexer or parser.** A table-driven tool would add a build
  dependency, and error recovery and column-accurate locations are the two
  things generated parsers are worst at.
* **A token pushback buffer.** The grammar needs one token of lookahead, which
  the parser has as the token it holds; nothing needs two.
* **A parse tree for expressions.** There are no expressions. Arithmetic is
  deferred, and a comparison is two terms and an operator.
* **Recovery inside a statement.** Inserting a missing parenthesis and
  continuing produces a tree the writer did not write. Recovery discards to
  the next terminator, which is the granularity the language already has.
* **An error limit.** Recovery bounds the message count by the number of
  statements, so a cap would only truncate a report that is already
  proportional to the mistakes in the file.
