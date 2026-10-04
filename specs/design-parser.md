# relic parser design

**Status.** Design document for the front end: the byte-oriented lexer, the
recursive-descent parser, the abstract syntax tree they produce and the
diagnostics they emit. Forward-looking, like every document under `specs/`;
`TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 5 through 13, 17, 18, 50, 56
and 67; `specs/design-memory.md` for the arenas and the interning tables;
`specs/decisions.md` D-1, D-2, D-4, D-5, D-6, D-12, D-15 and D-16.

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
inside one it is data and is not classified at all. Two of them, `#` and `$`,
are reserved as well as rejected, D-30: `$` followed by a name is kept for a
parameter, a value supplied from outside the program, and `#` followed by a
name for a built-in constant or a directive, so neither may be given another
meaning.

### 3.2 Tokens

```text
symbol          a lowercase-initial identifier: [a-z][A-Za-z0-9_]*
variable        an uppercase- or underscore-initial identifier:
                [A-Z_][A-Za-z0-9_]*
string          "..." with escapes, holding arbitrary bytes
integer         [0-9]+, its magnitude, converted to int64_t by the parser
operator        + - * /, one token kind carrying which
compare         = != < <= > >=, one token kind carrying which
implies         :-
( ) { } , . ?   punctuation
end of input
error           a byte sequence that is not a token, including the
                floating-point forms section 3.5 reserves
```

Identifiers are ASCII, so `$` is not an identifier byte and a generated name
containing one cannot collide with a name a user could write.

Comparison operators are one token kind with an operator field rather than six
kinds. The parser never wants to know which operator it has until it stores
it, and a single kind keeps the token switch from listing six cases that do
the same thing. The arithmetic operators are one kind for the same reason;
the remainder operator is the symbol `mod`, recognized by position, and `%`
still begins a comment everywhere, `specs/design-arithmetic.md` section 3.1.

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

`count`, `sum`, `min` and `max` are ordinary symbol tokens by the same
reasoning, and unlike `not` they cost no name. The parser reads one as an
aggregate function only after the `=` of an equality and followed by `(` or
`{`, which one token of lookahead decides, since a symbol in operand
position is never otherwise followed by either. Anywhere else the word is
the constant or the predicate name it always was, so `status(t, count).`
stores a symbol and a predicate may be called `count`. The aggregate
literal and what the parser checks of it are specified in
`specs/design-aggregates.md` section 3.

The names of the six string functions, `length`, `cat`, `substr`, `index`,
`match` and `str`, are ordinary symbol tokens by the same reasoning. A name
followed by `(` is a call only where an operand may stand, so
`length(a, 4).` is a fact of `length/2` and `p(X) :- length(X).` reads a
predicate named `length`; `specs/design-strings.md` section 3.2.

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

A literal is `-?[0-9]+` whose value fits `int64_t`, base ten. A value
outside the range is an error that names the literal, so that
`-9223372036854775808` is representable and `9223372036854775808` is
diagnosed rather than wrapped. A leading zero is accepted: `007` is 7, and
`-0` is 0.

The literal is two tokens' work. The lexer reads the digits as a
magnitude, and **the sign is the parser's**: a `-` is always a token of its
own, since with arithmetic it may be a subtraction, and the parser joins a
`-` followed by an integer token into a negative literal wherever a term or
an operand may stand, applying the range check with the sign known.
`offset(start, - 1).` is therefore the same fact as `offset(start, -1).`,
and `X - -1` is `X - (-1)`; `specs/design-arithmetic.md` section 3.4.

The table importer (`specs/design-import.md` section 4) reads a field
under the same rule through `relic_lex_integer()`, one function over a
byte range composed of the two pieces the lexer and the parser each use: a
magnitude conversion over a digit range, `relic_lex_magnitude()`, and the
signed range check. It reads nothing past the range, and when it refuses
it says which way: the range is not an integer, or it is one outside
`int64_t`, since the importer reports the two differently.

An identifier byte immediately following the digits is an error rather than
the start of a new token. Without the check, `0x10` would lex as the integer
`0` followed by the symbol `x10`, and the complaint would be about a missing
comma several bytes later.

Digits followed by a period and a digit are reserved for a floating-point
literal, D-30, whose syntax `specs/design-floats.md` section 2 gives. Digits
followed by an exponent -- `e` or `E`, an optional sign and a digit -- are
rejected with them, since the writer means a number and no float is spelled
without its period. Until the type exists the lexer rejects either form with
one message, at the number's first digit rather than at the byte after the
digits, since the writer meant one number:

```text
rules.dl:4:12: error: floating-point numbers are not supported
```

The rejected token is the whole form, digits, period, digits and exponent, so
the period inside `1.5` is not taken for the terminator at which the parser
resumes, and the statement reports once, as a string literal with a bad escape
is consumed whole for the same reason. A period after digits followed by
anything but a digit is the terminator it always is: `p(1).2 > 1?` is a fact
and a query, `X = 1. 2 > 1?` ends a statement at the period, and so does a
period followed by a comment or a newline. `1e` and `1ex` are not exponents
and get the identifier-byte error above, at the `e`.

An integer is therefore the one token after which the lexer looks past what it
has recognized, deciding on at most a period and a digit, or an `e`, a sign
and a digit, and it does so to make a misspelled literal report itself and to
keep the floating-point spelling free. The table importer is unaffected:
`relic_lex_magnitude()` refuses any byte that is not a digit, so a field `1.5`
or `1e5` is not an integer there either.

### 3.6 The input

The lexer scans a byte range and reads nothing past its end, so a caller
hands it text of any length with no terminator. A NUL *inside* the range is
an ordinary byte and is not a token, so it is reported as an unexpected
character.

---

## 4. Locations

Every token carries a `relic_location_t`: the source name, and the line and
column of the token's first byte, both counting from one.

Columns count bytes rather than characters or display cells. A tab is one
column. Anything else would require the lexer to know an encoding and a tab
stop, and the number is there to let a reader find the byte, not to align a
caret under it.

The source name is interned in the symbol pool and the location holds the
pointer the pool returns. Those bytes live in the pool's string arena and
never move, so a location stored in the tree stays printable for the life
of the context, and loading the same file twice stores one copy of its
name.

---

## 5. The tree

`lib/ast.h` defines the types below. They are a transcription of the grammar of
architecture section 13 and deliberately nothing more: no predicate table, no
variable numbering, no reordering. What the parser accepts is a syntactic
question, and what it produces should still be recognizable as the text.

```c
term      := a constant's domain identifier, or a variable
atom      := predicate name, arity, terms
literal   := a positive atom, a negated atom, a comparison, or an aggregate
comparison := two expressions and a comparator
expression := a term, a negation of one expression, an operator over two,
             or a call of a function over one to three expressions
statement := a rule (head plus a possibly empty body), or a query (a body)
program   := a list of statements
```

**A fact is a rule with an empty body.** There is no third statement kind and
no `is_fact` flag, because either would be a second way to say something the
body length already says, and two representations of one fact eventually
disagree. `edge(a, b).` is a rule whose body has no literals, which is also
exactly what it means.

**The tree holds rules and queries, not the facts a load states.** A fact
whose terms are all constants is a tuple of its predicate's base store,
`specs/design-facts.md`: a load stages it (section 8) and keeps no
statement for it. The one fact the tree holds is a fact with a variable
among its terms, which cannot be a tuple; it is kept as a statement so that
the check rejects it as it rejects any unsafe rule, with the diagnostic an
unsafe fact receives. Text parsed as a query stages nothing, and a fact
there is kept as a statement and reported as a fact where a query was
expected.

**A term is a domain identifier or a variable.** A constant is interned as it
is parsed, so the tree holds the same 32-bit identifier a tuple column will
hold and no later stage re-derives it. A variable holds the interned symbol of
its name, for diagnostics and for the numbering the program model will do.

**Anonymous variables are marked, not recognized by name.** Every occurrence
of `_` is a distinct variable, while `_Unused` is an ordinary named one, so
the distinction is a flag on the term rather than a string comparison repeated
wherever variables are examined.

**An aggregate is a literal that holds a body.** It records its function,
its aggregated variable when the function takes one, its result variable,
and its group body, an array of literals as a rule's body is, none of them
an aggregate. Which variables of the body are shared with the rule is not a
parser question and is not in the tree; `specs/design-aggregates.md`
section 3.4 specifies the literal and section 5 what the program model
builds from it.

**A comparison holds two expression trees.** An expression is a
transcription of the text as the rest of the tree is: a node is a term, a
negation, an arithmetic operator over two nodes or a call of one of the six
string functions over its argument nodes, with the shape the writer gave
it, since it is what `-e` prints a rule from. A rule head, a fact and an
atom argument hold terms, never expressions; `specs/design-arithmetic.md`
section 3.3 says why, and `specs/design-strings.md` section 6 what a call
node holds.

**A body literal beginning `name(` is an atom or a call**, decided by the
token after its closing parenthesis: an operator, `mod` or a comparator
makes it the first operand of a comparison, and anything else leaves it the
atom it has always been. The arguments are parsed as expressions in both
cases and an atom takes each one-node tree as its term, so the decision
costs one token of lookahead and no backtracking;
`specs/design-strings.md` section 3.3.

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
| the name and arity of a fact a load stages | predicate identifier | the parser |

The lexer interns *bytes* and the parser interns *values*, and the division is
not arbitrary. An identifier token does not yet know whether it names a
predicate or is a symbol constant, and interning it as a value on sight would
give every predicate name a domain identifier it never uses. The lexer
therefore produces the symbol identifier, which both uses need, and the parser
produces the value only where a term is being built.

A staged fact's predicate is interned because staging and the base stores are
keyed by it, and a fact keeps no statement from which the program model could
intern it later. Every other atom's predicate is interned by the program model
when it compiles the statement, per `specs/design-program.md` section 3.

Interning is permanent and context-lifetime. A file that fails to parse leaves
its symbols and values interned, which costs a little memory and nothing else:
identifiers are assigned in first-appearance order, and first appearance
includes an appearance in a statement that was later discarded. A failed load
leaves the predicates of the facts it staged interned in the same way; one the
failed load first named holds no fact and no atom references it, so no
warning ever names it (`specs/design-facts.md` section 3.5).

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
not a backslash, and as `0x%02x` otherwise. The stderr formatter escapes what
it prints in any case, so this is about readability rather than safety. The
backslash is the exception because the formatter doubles it, so that its own
escapes cannot be mistaken for the text's: quoted as a character, a stray
backslash would be shown as two.

An expression adds messages of the same shape, each at the byte the reader
should look at: a constant operand of the wrong type at the constant, an
expression nested too deeply where the level opens, and a missing operand
in the ordinary "expected an operand" form,
`specs/design-arithmetic.md` section 3.5. A call adds two of its own at the
function's name, and keeps the one an aggregate name in operand position
has always had, `specs/design-strings.md` section 3.4:

```text
rules.dl:3:11: error: foo is not a function
rules.dl:5:9: error: substr takes 2 or 3 arguments, not 4
rules.dl:7:14: error: an aggregate is written V = count { ... }
```

An atom argument that is an operator node or a call is reported at that
node's byte with the message an atom has always given there, `expected ','
or ')', found '+'`, once the token after the atom has shown it is one.

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
int relic_load_file(relic_t *, const char *source_name, FILE *);
int relic_load_path(relic_t *, const char *path);
```

`relic_load_path()` opens the file and delegates; `relic_load_file()` reads the
stream into one buffer and delegates. A stream is read whole rather than
incrementally because the lexer scans one contiguous range, so every token,
however long, is a pointer and a length into the text with no buffer to
refill beneath it. An incremental reader would bound only the text: a load's
facts are staged rather than built into the tree (`specs/design-facts.md`),
and the staging, held until the load's all-or-nothing commit, grows with the
input regardless.

Multiple loads append to one logical program, which is what lets facts and
rules be split across files on a command line.

**A load is all or nothing.** Statements are parsed onto a list of the load's
own, and facts into the load's staging, a set of tuples per predicate. When
the whole input has parsed, the load commits its facts to their base stores,
all or nothing, and only when the commit succeeds splices its statements into
the program (`specs/design-facts.md` section 4.3). The commit measures and
reserves before it adds anything, so a tuple or memory limit the new facts
do not fit refuses the load with no base store grown and no statement
spliced:

```text
relic: limit exceeded: tuples=4 while loading e/1
```

naming the first predicate, in the order the load's facts name them, whose
base store the load would grow. A caller that ignores the return value is then
working with a program that never contains half of a file, and `relic -c` can
report every mistake in a file without the next stage having to wonder which
statements survived.

The staging is the load's working storage, released on the load's one exit
whether it failed to parse, was refused or committed, and is charged to no
limit: like the text itself, it is proportional to what the caller chose to
load.

What a failed load does keep is its arena memory, and the predicates its facts
named (section 6): the discarded statements were allocated from the program
arena and are not individually reclaimable. A program that loads a broken file
repeatedly grows slowly. This is worth stating rather than hiding, and it is
bounded by the size of the text the caller chose to load.

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

Aggregates do not nest. A group body is entered only from an aggregate, and
an aggregate met while one is already open is refused before its body is
entered, so the parser's descent through a group body is one level deep
however the text is shaped, and a program it accepts can be compiled and
printed without a stack check, `specs/design-aggregates.md` sections 3.4
and 14.

An expression is bounded twice by one constant, `RELIC_MAX_EXPRESSION_DEPTH`,
32, since each bound is a stack: the **descent of the text**, which counts
the parentheses, unary minuses and call parentheses the parser is inside
and bounds its own recursion, refused where the 33rd opens; and the
**height of the tree**, which counts every operator, negation and call node
and bounds every walk over the tree and the evaluator's stack, refused
where the node that would be the 33rd is written. `((((X))))` is one node
four levels down, and `cat(cat(...))` counts at both.
`specs/design-arithmetic.md` sections 3.5 and 13 and
`specs/design-strings.md` section 3.4.

Nothing else is bounded. A body may hold any number of literals, a file any
number of statements and an expression any number of nodes under its height;
all are bounded by the text the caller supplied.

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
* **Recovery inside a statement.** Inserting a missing parenthesis and
  continuing produces a tree the writer did not write. Recovery discards to
  the next terminator, which is the granularity the language already has. A
  mistake inside a group body is no exception: the discard runs to the next
  terminator, which the grammar never places inside the braces, so a group
  body is recovered with the statement that holds it and never on its own.
* **An error limit.** Recovery bounds the message count by the number of
  statements, so a cap would only truncate a report that is already
  proportional to the mistakes in the file.
