# relic string operations design

**Status.** Design document for string operations: the functions a program
may apply to strings inside a comparison, what each is worth over bytes,
the regular expression matcher relic carries for one of them, and what a
string the evaluator computes rather than reads does to the symbol pool,
the executor and a parallel round. Forward-looking, like every document
under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 6, 10.1, 13, 14, 19, 61,
63, 66, 69 and 74; `specs/design-parser.md` for the front end this extends;
`specs/design-arithmetic.md` for the expressions, the assignment, the two
kinds of slot and the pending values this design builds on, and which it
extends to a third kind of slot and a second kind of pending value;
`specs/design-memory.md` sections 7 and 8 for the symbol pool and the value
table; `specs/design-comparison-cost.md` section 7 for the identifier a
string value carries; `specs/design-plan.md` and `specs/design-planner.md`
for the operations; `specs/design-eval.md` for the executor and the batch;
`specs/design-parallel.md` for the frozen round a computed string must not
write into; `specs/design-magic.md` for the rewrite; `specs/decisions.md`
D-3, D-4, D-7, D-11, D-15, D-24 and D-26.

---

## 1. What this document settles

* which functions exist over strings, how a call is written, and where one
  may stand -- inside an expression, and so only in a comparison, as every
  expression is;
* how a call at the start of a body literal is told from an atom without
  reserving a single name, so that `length(X) :- ...` still defines a
  predicate and `length(S) > 3` is still a comparison;
* what each function is worth: bytes, never characters; positions counted
  from zero, with -1 for a position that does not exist; every function
  total over operands of the right type, so that a test and its negation
  are both plain comparisons; and what a function does with an operand of
  the wrong type, a result too long to be a value, and a regular expression
  that is not one;
* the regular expression dialect, why it is the size it is, and the matcher
  that decides it in time linear in the pattern times the text, on every
  platform alike, whatever the pattern's author intended;
* how a string the executor computes lives in the binding frame, reaches a
  head tuple and becomes an interned identifier without the executor
  writing to the context, on the mechanism `specs/design-arithmetic.md`
  built for integers, so that the frozen round keeps its contract and a
  declined merge leaves the symbol pool as it found it;
* what interning during evaluation costs the pool, and which limit bounds
  it;
* the diagnostics, the limits, the explain text and the tests.

It does not add a character type, a string aggregate, a conversion that can
fail, or a second syntax for tests. Section 19 says why each stays out.

---

## 2. Why strings, and why this much

`docs/todo-language.md` puts string operations first among what relic lacks
and other Datalogs have. Text processing is an intended use, and today a
string is an opaque value that can be compared and nothing else, so the
first program over file names or log lines cannot be written:

```prolog
% Sources under src/ that no test names.
untested(P) :-
    file(P),
    index(P, "src/") = 0,
    match(P, "\\.c$") >= 0,
    not tested(P).

% A module's object file, from its source file.
object(M, O) :-
    source(M, S),
    O = cat(substr(S, 0, length(S) - 2), ".o").

% Paths that climb out of their tree.
escapes(P) :-
    path(P),
    index(P, "..") >= 0.

% A message with a count in it.
report(T, Msg) :-
    task(T),
    N = count { blocker(T, _) },
    Msg = cat(cat("blocked by ", str(N)), " tasks").
```

Every example applies a function to values a positive literal bound and
compares or assigns the result. That is the feature: **a function call may
stand where a term stands in an expression**, and nothing else about the
language changes. Heads and atom arguments still hold terms, values are
still symbols, strings and integers, and a rule is still a conjunction of
literals over relations. The assignment of `specs/design-arithmetic.md`
section 4.4 gives a computed string its name, exactly as it gives a
computed integer one.

The functions are six:

```text
length(S)          the number of bytes in S
cat(A, B)          A's bytes followed by B's
substr(S, I)       the bytes of S from position I to the end
substr(S, I, N)    at most N bytes of S from position I
index(S, T)        the position of the first occurrence of T in S, or -1
match(S, R)        the position of the leftmost match of the pattern R, or -1
str(V)             V as a string: a string as itself, a symbol's spelling,
                   an integer in decimal
```

This is `awk`'s vocabulary -- `length`, `substr`, `index`, `match`,
concatenation -- and that is not an accident. `awk` is the tool relic
measures its installation and audit cost against, its model of a string is
the same sequence of bytes, and its way of asking a question is the same:
every operation is a function that yields a value, and a test is a
comparison over one. `index(S, T) >= 0` asks whether `S` contains `T`,
`index(S, T) < 0` asks whether it does not, `index(S, P) = 0` asks whether
`S` begins with `P`, and `substr(S, length(S) - length(X)) = X` asks
whether it ends with `X`. Souffle spells the first three as builtin
literals, `contains`, and negates them with `!`; section 19 says why relic
does not. What it comes to is that the shape "expression in a comparison"
already exists, already binds, already plans, already rewrites and already
prints, and a function is one more node in it, whereas a builtin literal is
a second kind of literal in every one of those places.

`str` is the one conversion, because it is the one that cannot fail: every
value has a spelling. Text is what a program builds from other values, and
a message that names a count needs the count as text. The conversions the
other way, a string to the symbol or the integer it spells, can fail on
their input and are left absent, section 19. The float type of
`specs/design-floats.md` section 8 adds two numeric conversions: `float`,
which cannot fail either, and `int`, which fails as an overflow does.

Architecture section 71 asks for no feature without a clear Datalog use,
and that is why the list stops here. Each of the things left out, from
`upper` to `split`, has a use somewhere; none has one that the workloads of
`specs/design-bench.md` or the programs of architecture section 4 need, and
each is a later function that this design leaves room for rather than a
decision it has to make now.

---

## 3. Syntax

### 3.1 Tokens

No token is added. A function name is a symbol token, `[a-z][A-Za-z0-9_]*`,
and the parentheses and commas of a call are the ones an atom uses.

### 3.2 Calls

The `primary` production of `specs/design-arithmetic.md` section 3.2 gains
one alternative:

```text
primary      := integer
              | variable
              | symbol
              | string
              | "(" expression ")"
              | call

call         := name "(" expression ("," expression)* ")"
```

A call is a primary, so it binds tighter than every operator and never
needs parentheses around it: `length(S) - 2` and `-length(S)` mean what
they read as. Its arguments are expressions, so `substr(S, I + 1, N * 2)`
and `cat(cat(A, B), C)` are calls, and a call may be an argument of a call
without limit but the nesting bound of section 3.4.

**A name is a function only where an operand may stand**, exactly as `mod`
is an operator only after an operand and `count` a function only after the
`=` of an equality. `length(S) > 3` holds a call because `length(` stands
where an operand does; `length(a, 4).` is a fact of `length/2`; and
`p(X) :- length(X).` reads a predicate named `length`. Nothing is reserved,
and calls take no program from another meaning: without them, a name
followed by `(` in operand position has none, and is refused with the
message that an aggregate is written `V = count { ... }`, which section 3.4
keeps for the four aggregate names.

The six names, their arities and what each returns are the parser's to
know, as the four aggregate names are: a call to a name that is not a
function, or with the wrong number of arguments, is a parse error at the
name, section 3.4. Every function has a fixed arity, at most three;
`substr` is two functions of one name, `substr/2` and `substr/3`, as `p/2`
and `p/3` are two predicates. Section 7.4 says what a fixed arity buys.

### 3.3 A call at the start of a body literal

A body literal that begins with a name followed by `(` is an atom in every
program that parses today, and it may now be the first operand of a
comparison: `length(S) > 3`, `index(P, "..") >= 0`, `cat(A, B) = C`. The
parser reads such a literal as an **atom or call** and decides which by the
token after its closing parenthesis:

```text
body-literal := "not" atom
              | name "(" [ expression ("," expression)* ] ")" continuation
              | expression comparator expression

continuation := (an operator, "mod" or a comparator) ...
                                      the call is the first primary of the
                                      left expression of a comparison
              | (anything else)       the literal is the atom, complete
```

**An operator or a comparator continues a call; anything else ends an
atom.** The parser does not hold a list of the tokens that may follow an
atom -- a comma, a period, a question mark, or the `}` that closes an
aggregate's body -- because the enclosing body parser already checks what
follows a literal, as it does for every literal today; the atom-or-call
reader only asks whether the next token is one that can continue an
expression, and that set is the expression grammar's, not a second one.
`mod` counts as an operator here by the rule that made it one: it is a
symbol token in a position where only an operator may stand.

The arguments are parsed as expressions in both cases, because the parser
cannot know which case it is in until it has read past them. Once it does
know:

* **an atom's arguments must be terms.** An argument that is an operator
  node or a call is reported at that node's byte -- the operator, or the
  `(` of the call -- as `expected ',' or ')', found '+'`, the message the
  arithmetic parser gives `q(X, Y + 1)` today, at the same byte. The
  wording does not change because the check moves from the argument parser
  to the atom's completion;
* **a call's name must be a function and its arity that function's**,
  section 3.4. A literal `r(X) = 3` where `r` is no function is reported
  as `r is not a function` at `r`, where it was `expected ',' or '.',
  found '='` at the `=`. Both are errors; the new one names the mistake a
  reader is likelier to have made.

An atom's zero-argument form, `initialized`, has no parentheses and is not
touched. `not` is followed by an atom and only an atom, as today, so `not
length(S) > 3` is the error it is now: a test's negation is the opposite
comparison, `length(S) <= 3`. A head and a fact are atoms and are parsed
by the rule they are parsed by today, so `object(cat(A, B)) :- ...` is
still `expected a term`.

The decision needs one token of lookahead past the `)` and no backtracking:
the arguments were parsed once, into expression trees, and an atom takes
each tree's one node as its term. That is the property `specs/design-parser.md`
section 2 asks of the parser, and it holds because every argument an atom
accepts is a one-node expression.

### 3.4 What is checked while parsing

The three checks of `specs/design-arithmetic.md` section 3.5 stand, and a
call is subject to each:

* **a constant argument of the wrong type** is reported at the constant, as
  a constant operand of `+` is: `length(42)` is `integer 42 cannot be an
  argument of length`, and `substr(S, "0")` is `string "0" cannot be an
  argument of substr`. A variable argument is not checked; its type is a
  property of the tuples that bind it, and section 4.3 settles what happens
  then. `str` accepts every constant;
* **nesting.** A call's `(` counts one level of the text's descent, as a
  parenthesis does, and a call node counts one level of the tree's height,
  as an operator does, so `cat(cat(cat(...)))` thirty-three deep is refused
  where its thirty-third `(` opens, and `RELIC_MAX_EXPRESSION_DEPTH` bounds
  every walk over a tree that holds calls as it bounds one that does not;
* **a missing argument or parenthesis** is the ordinary `expected an
  operand, found ')'` family.

Two checks are the call's own, each at the function's name:

```text
rules.dl:3:11: error: foo is not a function
rules.dl:5:9: error: substr takes 2 or 3 arguments, not 4
rules.dl:7:14: error: an aggregate is written V = count { ... }
```

The third is the message the parser gives today for an aggregate name in
operand position, kept, since `count(X)` where an operand stands is still
that mistake and not a call to a function named `count`.

An expression that parses is a tree as before; a call is a node with a
function and an array of argument trees, section 6.

---

## 4. Meaning

### 4.1 Bytes, not characters

A string is a sequence of bytes, D-15: the lexer passes bytes above 0x7f
through unexamined, a string may hold a NUL, and nothing in the engine
knows what UTF-8 is. The functions follow. **`length` counts bytes,
positions are byte offsets, `substr` cuts between bytes, and `index` and
`match` compare bytes.** `length` of a string holding one two-byte UTF-8
sequence is 2, and `substr` can cut that sequence in half, exactly as
`awk` under `LC_ALL=C` does and `cut -b` does.

A design that counted characters would have to decide what a character is
-- a code point, a grapheme, under which normalization -- in an engine
whose determinism rests on never consulting a locale, architecture section
61, and would have to decide what an invalid sequence counts as. Bytes have
one answer on every platform, and `length` of a string is what `wc -c`
says. A program over UTF-8 text that needs code points can be given a
function that counts them, section 19, without changing what any of these
six mean.

### 4.2 The functions

Every function is **total over operands of the types it names**: given
strings where it wants strings and integers where it wants integers, it has
a value for every input. Positions count from zero. A position that is not
in the string is clamped, and a position that does not exist is -1.

| Call | Value |
| --- | --- |
| `length(S)` | the number of bytes in `S`, an integer |
| `cat(A, B)` | the bytes of `A` followed by the bytes of `B` |
| `substr(S, I)` | the bytes of `S` at positions `p` with `I <= p`, in order |
| `substr(S, I, N)` | the bytes of `S` at positions `p` with `I <= p < I + N`, in order |
| `index(S, T)` | the least `p` such that `T`'s bytes are `S`'s bytes at `p` onward, or -1 if there is none |
| `match(S, R)` | the least `p` such that the pattern `R` matches some prefix of `S`'s bytes from `p` onward, or -1 if there is none |
| `str(V)` | `V` itself for a string; a symbol's spelling; an integer in decimal, with a leading `-` when negative and no leading zeros |

`substr` is defined by which positions it keeps, so that every integer is a
valid argument: `substr("abc", -5, 100)` is `"abc"`, `substr("abc", -1, 2)`
is `"a"`, since only position 0 lies in `[-1, 1)`, `substr("abc", 5)` is
`""`, `substr("abc", 1, -1)` is `""`, and `substr(S, I, N)` with `I + N`
past `INT64_MAX` keeps every position from `I` on, since "`p < I + N`" is
read over the integers and not over `int64_t`. The implementation
intersects `[I, I + N)` with `[0, length)` without ever forming `I + N` or
`-I`: an `I` at or past the length, or an `N` at most zero, keeps
nothing; a negative `I` keeps nothing when `I <= -N`, where `-N` is
defined because `N` is positive, and otherwise moves the start to zero
and replaces `N` by `N + I`, a sum of a positive and a negative integer
that cannot overflow; and from the start so fixed at most `length -
start` bytes are kept. No step forms a value outside `int64_t`, and each
keeps exactly the positions of the interval that lie in the string, so
the unit test of section 15 checks it against the definition at every
extreme, `INT64_MIN` for `I` included. Souffle's
`substr` and `awk`'s clamp too; a version that failed on an out-of-range
position would turn every suffix test into two literals.

`index` of the empty string is 0, since the empty string occurs at every
position and 0 is the least, and `index(S, T)` with `T` longer than `S` is
-1. `match` with the empty pattern is 0 for the same reason. `str` of an
integer is the text the output formats print, `specs/design-cli.md`
section 4, so `str(N) = "42"` and a printed `42` agree byte for byte.

A result is a string or an integer as the table says, and an integer result
is a 64-bit integer that always fits: a length is at most the pool's limit
on a string, 2^32 - 2, and a position is at most a length.

### 4.3 An operand of the wrong type, a result that is not a value, and a pattern that is not one

Three things can go wrong when a call is evaluated, and they are treated
as `specs/design-arithmetic.md` section 4.2 treats their arithmetic
counterparts.

**An operand is not of the type the function wants.** Predicate arguments
are untyped and a column may hold a symbol beside a string. **A call one of
whose operands is not of the type it wants has no value, and a comparison
or assignment over it is false**, so the binding is dropped -- the rule an
expression with a symbol operand follows, and an ordering comparison across
types. The note of verbosity level 2, `RELIC_F_NOTE_MIXED`, covers it as it
covers those, once per rule:

```text
paths.dl:4:22: note: operand of index is a symbol; the comparison is false
```

This is a defined answer over the language's three types: a symbol does not
contain `".."`, as a symbol is not greater than a hundred. `str` has no such
case, since it takes every type. A symbol is not silently read as its
spelling by the other five, because a symbol and the string of its spelling
are different values, architecture section 10.1, and a function that
blurred the difference on the way in would give `length(foo) = length("foo")`
where `foo = "foo"` is false; a program that means the spelling writes
`str`.

**A result is too long to be a value.** A string value holds at most
2^32 - 2 bytes, `specs/design-memory.md` section 7, and `cat` can exceed
that. **A call whose result would be longer than a string value can be is
an evaluation error**, as an integer result that does not fit is. The
evaluation stops where a limit would stop it -- no rows, a nonzero exit,
the relations stale so that the next evaluation starts from the facts --
and the diagnostic names the rule:

```text
build.dl:7:14: error: string too long in rule object/2
```

The argument for an error rather than a dropped binding is the arithmetic
design's, unchanged: through negation a dropped binding derives an
affirmative answer the program denies, and a string the machine cannot
hold has no answer the program means. No other function can fail this way:
`substr` and `str` produce at most as many bytes as they were given or
twenty, and the rest produce integers.

**A pattern is not a regular expression.** `match`'s second operand must be
a pattern of the dialect of section 4.5. **A pattern that is not is an
evaluation error** when it is met, for the same reason:

```text
paths.dl:9:22: error: invalid regular expression in rule c_file/1: unmatched (
```

A pattern written as a constant is met when the rule is compiled, section
6, and the error is reported there, at the literal, as a check error is;
so a program whose every pattern is a constant, which is nearly every
program, learns of a bad pattern before anything is evaluated. A pattern
that arrives in a variable is met when the call is evaluated over the
binding that supplies it, and the error is the executor's, section 8.5.

**What an evaluation error costs** is stated in `specs/design-arithmetic.md`
section 4.2 and holds here without change: whether an evaluation meets one
depends on which bindings its calls are evaluated over, so a strategy that
evaluates more bindings may fail where one that evaluates fewer succeeds,
every strategy that succeeds gives the same answer, and a query whose plan
holds a call is gathered rather than streamed, section 9.3.

### 4.4 Comparisons over computed strings

`specs/design-arithmetic.md` section 4.3 extends D-11's comparisons to a
computed integer. A computed string is compared the same way:

* two terms compare as they do today -- equality by identifier, ordering by
  decoded value, cross-type ordering false;
* a computed string against a computed string compares by bytes, under the
  language order of architecture section 10.1;
* a computed string against a term is equal to it exactly when the term is
  a string of those bytes, and ordered against it exactly when the term is
  a string; a term that is a symbol or an integer is unequal and unordered;
* a computed string against a computed integer is unequal and unordered.

So `cat(A, "") = A` holds for every string `A` and for no symbol, `str(X) =
X` holds for every string `X` and for no symbol or integer, and `length(S)
> foo` is false and noted. Equality between a computed string and an
identifier decodes the identifier to its bytes, since the computed side has
no identifier yet, section 8.2, and compares the bytes; it never interns
the computed side to compare identifiers, which would write the pool.

### 4.5 The regular expression dialect

`match` decides a pattern of this dialect, and no other:

```text
pattern   := branch ("|" branch)*
branch    := piece*
piece     := atom ("*" | "+" | "?")*
atom      := byte                   any byte but . [ ] ( ) | * + ? ^ $ \
           | "."                    any one byte
           | "[" set "]"            one byte of the set
           | "(" pattern ")"
           | "^"                    the start of the string
           | "$"                    the end of the string
           | "\" byte               that byte, whatever it is

set       := ["^"] item+            "^" first negates the set
item      := byte                   any byte but ] and \; "]" first is a byte
           | byte "-" byte          the bytes from the first to the second
           | "\" byte               that byte, whatever it is
```

Alternation binds loosest, then concatenation, then the three repetitions,
which are postfix and may stack: `a**` is `a*`, and `(ab)+?` is what it
reads as. `^` and `$` match at the start and the end of the string and
nowhere else: a string holds whatever bytes it holds, a line feed among
them, and is one string. `.` and a negated set match a line feed and a NUL
like any byte, since the matcher never asks what a byte means. A backslash
makes the next byte literal, so `\.` is a period and `\\` a backslash; the
escapes that spell a control byte, `\n` and `\xHH`, are the string
literal's, `specs/design-parser.md` section 3.4, and have already been
decoded by the time the pattern is a value. A pattern written as a literal
therefore doubles its backslashes, `"\\.c$"`, as one does in every language
that has both string escapes and regular expressions and no raw literal.

The pattern's parentheses nest at most `RELIC_MAX_EXPRESSION_DEPTH` deep,
32, for the reason expressions do: the pattern compiler descends once per
`(`, and a bound on the text is what bounds its recursion. A pattern with
an unmatched `(` or `)`, an unterminated `[`, a repetition with nothing
before it (`*a`, `(*)`, `a|*`), a range whose bounds are reversed, or a
trailing `\` is not a regular expression, section 4.3, and the message
names which:

```text
unmatched (          unmatched )          unterminated [
nothing to repeat    reversed range       trailing backslash
nested more than 32 levels deep
```

The last byte is named rather than shown because a diagnostic printer
doubles every backslash it prints, so that a message cannot be misread,
and `trailing \` would reach the reader as `trailing \\`.

**What is not in the dialect, and why.** Counted repetition, `a{3,5}`: it
is sugar, `aaaa?a?`, and the one construct under which the compiled pattern
is not linear in the written one, since `(a{100}){100}` is ten thousand
states from twelve bytes. Character classes, `[[:alpha:]]` and `\d`: they
name sets of characters, and there are no characters; `[a-zA-Z]` and
`[0-9]` say the same over bytes. Backreferences: they make matching
NP-hard, which no bound below survives. Captures and a match's extent:
`match` reports where a match begins, section 4.2, which `substr` can cut
from; the extent is a second value, section 19. Case-insensitive matching
and multi-line anchors: flags, on a locale-free engine that has no notion
of case beyond ASCII. Each is a construct the dialect leaves room for, and
none is needed to say what a file name or a log line looks like.

The dialect is POSIX extended regular expressions less those things, which
is what `grep -E` reads less those things, and a pattern that is valid
here means what it means there.

**What `match` reports** is the least position at which some match begins.
Only the position, and so only "leftmost", is needed from the matcher;
which of several matches beginning there is the match -- POSIX's longest,
Perl's first -- is a question about the extent, and the extent is not
reported. That is why the matcher of section 8.4 needs no priority rule and
no submatch tracking, and is a few hundred lines rather than a thousand.

### 4.6 Termination

A rule that computes a new string from an old one can derive a relation
with no fixed point in any practical sense:

```prolog
s("").
s(Y) :- s(X), Y = cat(X, "a").
```

The pool holds strings of at most 2^32 - 2 bytes, so this rule fails with
a string too long after four billion derivations, and long before that the
memory limit stops it, section 9.1, since each derivation interns a string
one byte longer than the last. Termination is the program's responsibility,
as `specs/design-arithmetic.md` section 4.6 made it for arithmetic, and the
tool's part is the tuple and memory limits, which stop such a program with
a diagnostic. A recursive rule that computes a string should carry a bound,
and the manual page says so where it introduces the functions.

---

## 5. Safety

Nothing changes. A call is an expression, and `specs/design-arithmetic.md`
section 5 states safety over expressions: an equality whose one side is a
bare variable not yet bound and whose other side's slots are all bound is
an assignment, and the closure over assignments is what makes every plan
placeable. `O = cat(substr(S, 0, length(S) - 2), ".o")` assigns `O` once
`S` is bound, and `match(P, R) >= 0` is a filter placeable once `P` and `R`
are. The diagnostic for an unbound variable is the one that names an
assignment as the second way to bind, and the singleton warning covers a
computed string nothing reads.

The generator of `specs/design-hardening.md` section 2.4 draws a call's
arguments from the variables its positive literals bound and from
constants, as it draws an operator's, so a generated call is safe by
construction, section 15.

---

## 6. The compiled rule

The expression node of `specs/design-arithmetic.md` section 6 gains one
kind:

```c
enum relic_expr_kind {
	RELIC_EXPR_CONSTANT,
	RELIC_EXPR_VARIABLE,
	RELIC_EXPR_NEGATE,
	RELIC_EXPR_BINARY,
	RELIC_EXPR_CALL			/* fn(args) */
};

enum relic_function {
	RELIC_FN_LENGTH,	/* string -> integer */
	RELIC_FN_CAT,		/* string, string -> string */
	RELIC_FN_SUBSTR2,	/* string, integer -> string */
	RELIC_FN_SUBSTR3,	/* string, integer, integer -> string */
	RELIC_FN_INDEX,		/* string, string -> integer */
	RELIC_FN_MATCH,		/* string, pattern -> integer */
	RELIC_FN_STR		/* any -> string */
};

struct relic_expr_call {
	relic_expr_t		*args[RELIC_MAX_FUNCTION_ARITY];	/* 3 */
	const relic_pattern_t	*pattern;	/* MATCH with a constant */
	enum relic_function	 fn;
	uint8_t			 argc;
};
```

A call node lives in the program arena beside its rule, as every node does,
and its height and node count are recorded as every node's are: height one
more than its tallest argument, nodes one more than its arguments' together.
The Magic rewrite's limit, which counts sizes, needs no change to charge a
call what it costs, section 10.

**A constant pattern is compiled with the rule.** When the rule compiler
resolves a `match` whose second argument is a string constant, it compiles
the pattern once, into the program arena beside the node, and the node
keeps a pointer to the compiled form. A pattern that does not compile is a
check error at the literal, section 4.3, and the statement is refused as an
unsafe rule is. The compiled pattern lives exactly as long as the rule, and
a plan's step points at it rather than compiling again, so a constant
pattern costs its compilation once per program rather than once per
binding. A generated rule that copies the node copies the pointer; the
program arena outlives every query and every rewrite.

A `match` whose pattern is a variable has no compiled form in the rule; the
executor compiles the pattern when the call runs, section 8.4.

The function's argument and result types are a property of the function,
known to the parser, the rule compiler, the planner and the executor from
one table in `lib/strfn.c`, so that no consumer of a call carries its own
list of what `substr` takes.

---

## 7. Planning

### 7.1 Placement

Nothing changes. `specs/design-arithmetic.md` section 7.1 places an
assignment once its expression's slots are bound and a filter once every
slot it reads is, and a call is part of an expression. The greedy order of
positive literals is untouched, and a positive literal keyed on a slot a
string assignment bound has a bound argument, as one keyed on a computed
integer does.

### 7.2 The third kind of slot

`specs/design-arithmetic.md` section 7.2 gives a slot a kind the plan fixes:
a **domain slot** holds an identifier read from a tuple, a **number slot**
holds an integer the executor computed. A string the executor computed has
no identifier yet either, and its value is bytes, so it needs a kind of its
own:

* a **text slot** is bound by an assignment from an expression whose value
  is a string the executor computed -- one whose top node is a `cat`, a
  `substr` or a `str` -- or from another text slot; its `texts` entry says
  where its bytes are in the executor's text stack, section 8.1, and its
  frame entry is the string's identifier when the context holds one and
  zero otherwise, section 8.2.

An assignment from `length`, `index` or `match` binds a number slot, since
its value is an integer the executor computed; an assignment from a
constant or a domain slot binds a domain slot, as today. The kind is
static, decided when the plan is built, written into every read of the
slot, and never asked at run time: the term-action rule of
`specs/design-plan.md` section 4 applied to one more fact. The result type
of a call is a property of its function, section 6, so the planner reads
it from the same table the parser checked arguments against.

### 7.3 Access paths

A positive literal whose bound columns include a text slot is looked up,
tested or scanned through the slot's frame entry, exactly as one keyed on
a number slot is: an identifier when the pool holds the string, zero when
it does not, and zero matches no tuple, section 8.2. `object(M, O) :-
source(M, S), O = cat(substr(S, 0, length(S) - 2), ".o"), exists(O)` looks
`exists` up on `O`, a text slot, with the key built from its frame entry.

### 7.4 The operations and their steps

No operation is added. `FILTER` and `ASSIGN` run postfix programs, and the
program gains two kinds of step:

```c
enum relic_step_kind {
	RELIC_STEP_CONSTANT,		/* push an interned value */
	RELIC_STEP_DOMAIN_SLOT,		/* push a domain slot's identifier */
	RELIC_STEP_NUMBER_SLOT,		/* push a number slot's integer */
	RELIC_STEP_TEXT_SLOT,		/* push a text slot's bytes */
	RELIC_STEP_NEGATE,
	RELIC_STEP_BINARY,
	RELIC_STEP_CALL			/* pop argc cells, push the result */
};

struct relic_step {
	union {
		relic_dom_t		 constant;
		uint16_t		 slot;
		enum relic_arith_op	 op;
		struct {
			const relic_pattern_t	*pattern;	/* or NULL */
			enum relic_function	 fn;
		}			 call;
	}			u;
	enum relic_step_kind	kind;
};
```

A `CALL` step is emitted after its arguments' steps, so a program is still
its tree in postfix order and still runs on a stack by a loop with no
recursion. The step carries the compiled pattern of a constant `match`,
section 6, so that the executor never asks whether an argument was a
constant.

**The stack's depth is computed, not bounded by height alone.** A binary
node needs its left operand's cell while its right is evaluated, which is
why a tree of height *h* needed *h* + 1 cells. A call needs each earlier
argument's cell while a later one is evaluated: the cells a node needs are
`max(need(arg_i) + i)` over its arguments, counting `i` from zero, and a
`substr` nested in its own third argument needs two more cells per level.
The walk that emits a program computes the need by that recurrence. A tree
of height *h* needs at most 2*h* - 1 cells under it, since a leaf needs one
and each level adds at most two, so the executor's stack has
`RELIC_POSTFIX_DEPTH_MAX` cells, now `2 * RELIC_MAX_EXPRESSION_DEPTH - 1`,
which the recurrence cannot exceed for functions of at most three
arguments and a tree of height at most 32.
That is what a fixed arity buys: a variadic `cat` would need a stack cell
per argument, and either a second bound on the argument count or a stack
sized per plan. `cat(cat(A, B), C)` costs the writer six characters and
the design nothing.

An `EMIT`'s head columns gain one action beside the five of
`specs/design-arithmetic.md` section 7.4:

```c
enum relic_term_action {
	RELIC_PLAN_CONSTANT,
	RELIC_PLAN_BOUND,
	RELIC_PLAN_NUMBER,	/* a number slot: its identifier, or pending */
	RELIC_PLAN_TEXT,	/* a text slot: its identifier, or pending */
	RELIC_PLAN_BIND,
	RELIC_PLAN_ANY
};
```

`TEXT` occurs only in an `EMIT`, as `NUMBER` does. Every other operation
reads a text slot through `BOUND`, section 8.2, and a generator never binds
one.

---

## 8. Execution

### 8.1 Evaluating a call, and the text stack

A cell of the executor's stack holds one of three things:

```c
struct relic_cell {
	union {
		relic_dom_t	dom;
		int64_t		number;
		relic_text_t	text;		/* offset and length in the text stack */
	}		u;
	enum relic_cell_kind	kind;		/* DOM, NUMBER or TEXT */
};
```

`CONSTANT` and `DOMAIN_SLOT` push an identifier, `NUMBER_SLOT` an integer,
and `TEXT_SLOT` a text: an offset and a length in the **text stack**, a
byte array in the executor scratch that holds every string the executor has
computed and may still read. A `CALL` step pops its arguments and reads
each as the type the function wants: a cell that holds an integer is that
integer; a cell that holds a text is those bytes of the text stack; a cell
that holds an identifier is decoded -- an integer through the integer
table, a string to its bytes through `relic_symbol_bytes()`, which is
`const` and reads the pool's string arena where bytes never move -- and a
cell that decodes to the wrong type is **no value**, which ends the run as
a symbol operand of `+` ends one: the operation rejects the binding and
records the note of section 4.3. `str` reads any cell and never has no
value.

A function whose result is an integer pushes a number cell. A function
whose result is a string appends the result's bytes to the text stack and
pushes a text cell naming them. The bytes are built in place: `cat` copies
its two operands' bytes to the top, `substr` copies the range it keeps, and
`str` formats an integer with the renderer the output formats use or
copies a spelling. A result longer than a string value can be is the error
of section 4.3, recorded as an overflow is, section 8.5, before a byte of
it is copied.

**Each level of the plan owns the text stack above the top its
predecessor left.** A plan is nested loops, and an operation at level *L*
runs again each time a generator above it produces a new match, after
which every operation below *L* runs again too. The scratch keeps one
**text top** per level. Every operation, of every kind, begins each run by
cutting the stack to the top recorded for level *L* - 1 (zero for the
first level), builds whatever texts its programs compute above that, and
when it succeeds records the top for level *L*: an `ASSIGN` that binds a
text slot moves its result down to the top it inherited, points `texts[slot]`
at it and records the top past the result; every other operation -- a
`FILTER`, an `ASSIGN` of a number or domain slot whose expression built
texts on the way, a generator, an `EMIT` -- records the top it inherited,
so its temporaries are dead the moment it is done. An operation that
fails records nothing; its successor never runs, and its own next run cuts
the stack again.

The rule is one rule for every operation and it is what makes every read
safe: a text slot bound at level *B* is read only by operations at levels
past *B*, which run after *B*'s run on the same bindings and cut only to
tops at or past *B*'s recorded top, so the slot's bytes lie below every
cut a reader can make; and when a generator above *B* rebinds, *B* runs
again and rewrites both its bytes and `texts[slot]` before any reader
runs. Nothing is saved on descent or restored on backtracking, for the
reason no frame slot is, and no temporary outlives the operation that
built it, whatever the kind of that operation or the way it ended.

The text stack starts at `RELIC_TEXT_STACK_INITIAL` bytes, 4 KiB, allocated
when the evaluation begins with the rest of the scratch, and grows when a
result does not fit, doubling until it does, charged to the memory limit
with the rest of the evaluation's working state. Doubling rather than the
growable-array policy of `specs/design-memory.md` section 5, which raises
a growth to the request, makes the capacity a function of the largest
request alone, which the merge's admission below relies on. A growth the
limit or the allocator refuses fails the evaluation as an allocation failing in the executor does,
`specs/design-eval.md` section 8, and in a parallel round abandons the job,
`specs/design-parallel.md` section 4.4. A plan holding no string function
never touches the stack, and one whose strings are short never grows it.

**The buffers the data sizes are round-scoped, in every mode.** Four
buffers of a scratch are sized by the data a plan meets rather than by
the plan: the text stack, the pending bytes of section 8.3, and the
pattern buffer and matcher work space of section 8.4. Every other part of
a scratch is sized when the evaluation begins, from the plans, and is the
same at every thread count. These four are allocated at their initial
sizes when a round begins and released, their charge returned, when it
ends, whether the round ran on the serial evaluator's scratch or on the
workers' and whether it was merged, declined or abandoned; a query's plan
run outside any round owns its buffers the same way. A worker's four are
released when its task finishes, before the merge puts its admission
question; the candidate buffers, which hold the round's tuples and
pending bytes, are released after the merge's last phase, as
`specs/design-parallel.md` section 8 has them, since the commit reads
them. So at the start of every round, and at the start of the serial
rerun of a round that was declined or abandoned, the charged total is
the one a run that had been serial from the start would hold, and the
rule of `specs/design-parallel.md` section 8, that every refusal a
program can see is the serial evaluator's, is not broken through the
accounting. The cost is one small allocation per buffer per round.

Within a round the serial evaluator inserts as it derives, asking the
limit with its four buffers charged at whatever capacity they have grown
to, while the merge asks once, with the workers' buffers already
released. **The merge's admission therefore adds the capacity the serial
scratch's buffers would hold**, beside the relations' and the pool's
growth: for each of the four, the largest capacity any worker's buffer
reached, read from the workers before their buffers are released. That
is at least the serial run's capacity at the round's end, since a buffer
that doubles until it fits has a capacity that is a monotone function of
the largest request made of it, every worker's starts where the serial
run's does, and the tasks together make every request the serial run
makes. For the text stack and the matcher's buffers they make exactly
those requests; for the pending bytes they may make larger ones, since a
string the serial run interned earlier in the round is no longer pending
there while a worker, reading the frozen pool, still carries its bytes.
A merge declined on that excess is run again serially, where the limit
judges it exactly. And the serial run's capacity at the round's end is
at least what it had at any insertion of the round. Admission is then at least as strict as every
check the serial evaluator makes in the round, so an admitted merge
cannot fail where the serial evaluator would have refused, and a declined
one is rerun serially from the serial state, which is the contract the
merge already keeps for the relations.

A comparison of two cells is the rule of section 4.4: two identifiers as
today; two integers numerically; two texts by bytes; a text and an
identifier by decoding the identifier, equal or ordered only if it decodes
to a string; a text and an integer unequal and unordered. An `ASSIGN` binds
its slot from the cell left: an identifier into a domain slot, an integer
into a number slot, or a text into a text slot, by the slot's kind, which
the planner fixed from the program's result kind.

### 8.2 A computed string in the frame

A text slot's bytes are named by `texts[slot]`. Its frame entry is set at
the same moment by **one read-only probe of the pool**:
`relic_value_find_string()`, the lookup half of interning a string value,
which returns the identifier the context has assigned to a string of those
bytes, or zero when it has assigned none. The function is `const` over the
context, joins `lib/frozen.h` beside `relic_value_find_integer()`, and a
task of a parallel round may call it.

"Assigned" is exact: the pool may hold the bytes -- as a predicate's name,
a variable's spelling, or the spelling of a symbol interned as a symbol
value -- without the context ever having assigned a string identifier to
them, since `specs/design-memory.md` section 8 assigns a string identifier
by setting the string bit of the pool entry, and only interning a string
value sets it. A pool entry whose string bit is clear names no string
value, no tuple holds `RELIC_DOM_STRING | id` for it, and
`relic_value_decode()` refuses that identifier. So the probe finds the
entry and reads the bit, and returns zero unless the bit is set. `str(foo)`
over a symbol `foo` that no string literal spelled is therefore pending,
section 8.3, though its bytes are in the pool; what it lacks is the bit,
and setting the bit is a write.

Zero in the frame is what makes every later read of the slot need no case
of its own, and the argument of `specs/design-arithmetic.md` section 8.2
holds byte for byte: a generator or membership test reading the slot
through `BOUND` builds a key or compares a column with zero, which no
tuple holds, so a string the context has not assigned matches nothing --
correct, since such a string is in no relation; an expression reading the
slot pushes its text, never the frame entry; and an `EMIT` reading it
through `TEXT` writes the frame entry when it is not zero and otherwise a
pending string.

### 8.3 A computed string in a head tuple

A head column read through `TEXT` whose frame entry is zero names a string
the context has not assigned, so it has no identifier to write into the
batch. The `EMIT` copies the bytes into the batch's **pending bytes**, adds
an entry to the batch's **pending** array, and writes a **pending marker**
in the column: the entry's position, counting from one, with type bits of
zero, exactly the marker a pending integer leaves. The pending array of
`specs/design-arithmetic.md` section 8.3 holds, per entry, either an
integer or a byte range:

```c
struct relic_pending {
	union {
		int64_t		integer;
		relic_text_t	text;	/* offset and length in the pending bytes */
	}			u;
	enum relic_pending_kind	kind;
};
```

One array and one marker space for both kinds, so that the rewrite of a
batch is one loop over its columns and a column's marker says which entry
and nothing else. A marker never reaches a relation, a hash the store uses
or a callback, because every batch is resolved before it goes anywhere, and
a plan that streams never holds a text slot, section 9.3.

**The batch is full when its tuples are, or its pending bytes would be.**
The pending array is sized when the evaluation begins for the widest head
times the batch length, as today, so no `EMIT` allocates an entry. The
pending bytes cannot be sized in advance, since a string may be any
length, so the batch has a second fullness. The `EMIT` sums the bytes of
the tuple's pending strings before it writes anything; when they do not
fit the room the pending bytes have left and the batch holds a tuple, the
executor returns the batch to its caller as it does when
`RELIC_INSERT_BATCH` tuples are held, with the `EMIT` recorded as the
operation to ask again on the same bindings, and the caller resolves the
batch and empties both; asked again, the `EMIT` finds an empty batch and
writes its tuple once. The pending bytes are allocated to
`RELIC_PENDING_BYTES`, 1 MiB, when the evaluation begins and grow only
for one tuple that does not fit an empty batch, doubling as the text
stack does and charged like it; so a head whose strings are
ordinary never allocates in an `EMIT`, one whose strings are enormous
grows the buffer to the largest tuple and no more, and no tuple is ever
written twice or skipped.

**A string's identifier is known before it is assigned.** The pool assigns
symbol identifiers in order, `count + 1` to the next new byte string, and a
string value's identifier is `RELIC_DOM_STRING | id`; so the identifier a
new string will receive is a function of the pool's count and of the order
in which new strings are met, exactly as an integer's is a function of the
integer table's count. The three steps of `specs/design-arithmetic.md`
section 8.3 -- measure, rewrite, commit -- take a pending entry of either
kind:

1. The **measure** walks the pending entries in order and fills the map:
   for a pending string, the identifier the context has assigned to those
   bytes, when it has; else, when the pool holds the bytes with the string
   bit clear, `RELIC_DOM_STRING | id` for the entry it holds, which the
   commit will make a string value by setting the bit; else
   `RELIC_DOM_STRING | (count + j)` for the *j*th distinct new byte string
   met, distinct by bytes, which the map's `seen` table decides by hashing
   the bytes under the context's seed as the pool does. It returns the bytes
   the pool would grow by to hold the new strings -- entries, identifier
   table and string arena, at the **stepwise capacity** each would reach
   appending the strings one at a time, computed by the growth policy's own
   pure functions -- together with the growth of the value table's
   **string bits**, and the integer table's growth beside both. The bits
   are an array of their own, `specs/design-memory.md` section 8, grown to
   cover the highest symbol identifier that has a bit, and a string whose
   bytes the pool already holds under a high identifier -- a predicate's
   name, say -- can need the bits grown while needing no pool entry; so the
   measure projects the bits' growth from the highest identifier the map
   promises, new or existing, and not from the count of new strings. It
   reads the pool and the bits and writes only the map.
2. The **rewrite** writes each marker's identifier from the map over the
   marker, as today.
3. The **commit** reserves room in the pool, in the string bits and in the
   integer table at once, the one step that can fail, and then appends:
   each new string's bytes into the string arena and its entry and slot
   into the pool, under exactly the identifier the map promised, and sets
   the string bit of every entry the map promised an identifier for, new
   or not, counting each whose bit was clear as a string value for
   `relic_stats()`. Setting a bit cannot fail, because the reservation
   covered the highest identifier the map holds; that is why the bits are
   reserved with the pool and not grown as they are set. A commit that
   fails leaves the pool and the bits as they were.

The map's `fresh` array holds, per new byte string, its range in the
pending bytes, and the pending bytes stay where they are until the commit
has copied them, which is why the batch is not emptied until it is
resolved. The serial evaluator, taking a batch, measures, asks the memory
limit with the bytes the measure returned, refuses as an insertion
refuses, naming the predicate being derived into, and otherwise rewrites,
commits and inserts, as it does for integers; the parallel merge runs the
three steps at the three points section 9.2 gives.

### 8.4 The pattern matcher

`lib/regex.c` compiles a pattern of section 4.5 to a program and runs the
program over a string. It is relic's own, a few hundred lines, and section
19 says why it is neither `<regex.h>` nor a vendored engine.

**Compilation** is one recursive descent over the pattern's bytes, following
the grammar of section 4.5 with the parenthesis depth bounded, into an array
of instructions:

```c
enum relic_regex_op {
	RELIC_RE_BYTE,		/* consume one byte equal to b */
	RELIC_RE_ANY,		/* consume any one byte */
	RELIC_RE_SET,		/* consume one byte in a 256-bit set */
	RELIC_RE_SPLIT,		/* continue at x and at y */
	RELIC_RE_JUMP,		/* continue at x */
	RELIC_RE_BOL,		/* succeed only at position 0 */
	RELIC_RE_EOL,		/* succeed only at the end */
	RELIC_RE_MATCH
};
```

Concatenation emits its pieces in order; `a|b` is a split to each branch
with a jump past the second; `a*` is a split to `a` or past it with a jump
back; `a+` is `a` then a split back or on; `a?` is a split to `a` or past
it. Every construct of the dialect emits at most three instructions per
byte of pattern, so a program has at most `3 * length + 1` instructions and
its size is known from the pattern's before a byte is compiled, which is
what lets a compiled constant pattern be laid out in the program arena in
one allocation, and a compiled variable pattern in the scratch's
**pattern buffer**, grown once to that size. A set is a 32-byte bitmap in
the instruction, so an instruction is 40 bytes at most, and a pattern of
*n* bytes compiles into at most `120 * n + 40` bytes. A compiled program
is **immutable**: the matcher reads it and writes nothing into it, which
is what lets a constant pattern's program sit in the program arena and be
run by every worker of a round at once.

**The matcher's work space is the scratch's.** The two thread lists and the
marks are mutable, and they belong to the executor scratch that runs the
match, one set per worker, never to the program: a program shared by
sixteen workers has sixteen work spaces. A thread is two 32-bit words, the
instruction's index and the position its match began at, both of which
fit, since a program has fewer than 2^32 instructions and a string fewer
than 2^32 bytes; a mark is one 32-bit stamp. Each list has room for one
thread per instruction and the marks one stamp per instruction, so the
work space is five 32-bit words, twenty bytes, per instruction. It is
empty when a round begins and grown, doubling as the text stack does,
for the first match whose program is larger than it holds, and charged
like the text stack. The pattern buffer grows and is charged the same
way. A pattern of *n* bytes needs `120 * n + 40` bytes of pattern buffer
and `60 * n + 20` of work space, and a buffer that doubles holds less
than twice what it was asked for, or eight instructions' worth when that
is more, so a variable pattern of a million bytes can cost its worker
some 360 MB of charged scratch for the duration of one `CALL`, which the
memory limit sees and may refuse, and which section 13 lists.

**Matching** is Thompson's simulation, run to find the least position at
which a match begins. The state is two lists of threads, each thread an
instruction and the position its match began at; a list holds each
instruction at most once, kept by a per-instruction mark stamped with the
step's number, and the threads of a list are in nondecreasing order of
their start. The simulation walks the string one byte at a time. At each
position it first adds, while no match has been found, a thread at the
program's start whose start is that position; every thread already on the
list began earlier, so the new one comes last and the order is kept. Then
it steps the list over the byte, following splits and jumps with the marks
so that each instruction is added once, by the thread with the earliest
start, which is the first to reach it, and successors are appended in
their parents' order, so the next list is ordered too. A thread reaching
`MATCH`, while being added or stepped, records its start as the answer if
the answer is unset or larger. Once an answer is recorded no new thread is
added, and threads whose start is not less than the answer are dropped as
they are stepped, so the simulation ends when its list is empty, which is
at most one step past the string's end, where the last start thread is
added for a match of the empty suffix. The answer is then the least start
of any match, since every thread that could have begun earlier and matched
later was kept until it matched or died.

The simulation does a bounded amount of work per byte of the string -- at
most one visit per instruction -- so a match runs in time proportional to
the string's length times the program's, whatever the pattern: `(a*)*b`
over a million `a`s finishes in a million times a few steps, where a
backtracking matcher would not finish. There is no recursion in the
matcher, no allocation per byte and no allocation per match: the work
space is sized before the match begins and reset by stamping, not by
clearing, so a match over a short string against a large program does
not pay for the program's size per byte.

A variable pattern is compiled when the `CALL` runs, into the scratch's
pattern buffer, and its program is run once and discarded; a rule that
matches a million strings against the pattern in a column compiles it a
million times. That is stated rather than hidden, and section 19 records
a cache as the improvement to reach for when a workload shows it. A
constant pattern, which is the ordinary case, is compiled once, section 6.

### 8.5 Notes and errors cross the executor's boundary

The two records of `specs/design-arithmetic.md` section 8.4 serve:

* the **note** of section 4.3, for an operand of the wrong type, names the
  function in place of the operator, once per rule, with the same mark;
* the **error** of section 4.3, for a string too long or a pattern that is
  not a regular expression, is the error record with two more statuses
  beside overflow and division by zero, and for a pattern carries which
  of section 4.5's messages, so that the caller's diagnostic can say
  `unmatched (`.

The executor ends the plan as though every generator were exhausted, the
caller finds the record when it takes the batch, reports it, discards the
batch and fails the evaluation; in a parallel round the task abandons the
job and the round is run again serially, which meets the same error in
serial order and reports it with the serial message. Nothing is new here
but the statuses.

---

## 9. Interning what evaluation computes

### 9.1 One pool, charged

Computed strings go into the symbol pool of `specs/design-memory.md`
section 7, the one path every byte string takes, for the reason computed
integers go into the one integer table: a second pool would give the string
`"a.o"` computed by a rule and the string `"a.o"` written in a later load
two identifiers unless every interning consulted both in a fixed order.
One pool, one identifier per byte string, by construction; and a computed
string whose bytes are a symbol's spelling shares the symbol's pool entry,
as a literal of that spelling does, so `str(foo)` and `"foo"` are one
value.

What changes is who pays. The pool was left out of `max_memory` by
`specs/design-relations.md` section 9 because it was sized by what the
caller wrote, and with `cat` it is not: `s(Y) :- s(X), Y = cat(X, "a")`
interns a string per derivation. **The pool's storage -- its entry array,
its identifier table, its string arena and the value table's bits over it
-- is charged to `max_memory` whole**, literals and computed strings
alike, checked before each growth by the measure of section 8.3
and refused with the store's message:

```text
relic: limit exceeded: memory=536870912 while deriving s/1
```

The whole pool rather than the part evaluation added, for the reason the
integer table is charged whole: the account stays a sum of what exists.
A predicate's name is not in the pool; the name store holds it and is
charged as the pool is (`specs/design-vocabulary.md` section 3).

**The pool is not shrunk.** A string once interned keeps its identifier for
the life of the context, and a discard leaves the pool, as it leaves the
integer table; the memory limit bounds it with everything else. A string
a derivation computed was interned for the tuple that holds it, and
discarding the tuple does not remove the string. Reclaiming strings no
relation and no statement holds is left absent, section 19, with the
integer case. A collected result copies the bytes of the strings it holds
(`specs/design-vocabulary.md` section 4), so a caller keeps an answer's
strings without keeping the context.

### 9.2 The frozen round

`specs/design-parallel.md` section 7.1 has the compiler enforce that a task
cannot write the context, and `relic_symbol_intern()` is not in
`lib/frozen.h`. A `cat` that interned would not compile. Sections 8.1 to
8.3 are what an executor that cannot write does with a string it has just
made: keep it in its text stack, look it up read-only, and hand its bytes
to whoever stores the tuple.

The round's two screens are adjusted for a candidate with a pending string
column as `specs/design-arithmetic.md` section 9.2 adjusts them for a
pending integer: against the frozen store it is new and is not looked up,
since a string the context had not assigned when the round began is in no
relation the round reads; against the worker's own candidates it is hashed
and compared by bytes, through a per-worker table of the distinct pending
strings it has derived, so that equal strings hold equal markers and a
candidate is screened by value as by identifier. The worker's candidate
buffer carries the bytes of its pending strings beside its tuples, as the
batch does, charged as the buffer is.

The merge runs the three steps of section 8.3 at the three points the
arithmetic design fixed: the **measure** before anything is modified,
worker 0 walking every task's pending entries in task order into one map,
so that each new string's promised identifier is its position in the
serial order of first occurrence, with the pool's growth joining the
integer table's and the relations' in the one question put to the memory
limit; the **rewrite** still before anything is modified, so that every
point at which the merge may decline comes after it with the pool
untouched; the **commit** in the reservation phase, which can fail only for
memory and declines the round with the pool's contents unchanged, and the
appends in the phase that cannot fail. The bytes of the map's new strings
are read from the candidate buffers, which the merge holds until its last
phase.

**The identifiers are the same at every thread count.** The argument is
the arithmetic design's, with "integer table" read as "pool": the serial
evaluator interns a string when it commits the first batch holding it, the
merge when it commits the round with the identifier the measure promised,
which is its position in the serial order of first occurrence among the
candidates in task order, and that order is the serial derivation order
with duplicates removed. So the pool gains the same strings in the same
order, `relic_stats()` reports the same `string_values`, the pool reaches
the same capacity since both reserve the stepwise one, and `storage_bytes`
and every later memory-limit decision agree. The identity tests of section
15 assert `string_values` and `storage_bytes` with the rest.

### 9.3 Streamed rows

`specs/design-arithmetic.md` section 9.3 gathers a query whose plan holds
an arithmetic step, because such a plan can fail on any binding. **A plan
holding a `CALL` step is gathered** for the same reason, whatever its head
names: `cat` can fail on any binding, `match` on any binding whose pattern
is a variable, and a plan that is gathered when it holds a call and
streamed when it holds a call that cannot fail would be a list of
functions. So `file(P), index(P, "..") >= 0?` is gathered, and a streamed
batch never holds a pending string; "every batch is resolved before it goes
anywhere" needs no exception. A query with no call and no arithmetic
streams as it does today.

---

## 10. Magic Sets

Nothing changes in kind. The rewrite of `specs/design-magic.md` reads the
planner's order and the bindings it reports, copies comparisons into
prefixes by walking their expression trees, renumbers slots by the same
walk, and deduplicates generated rules by structural hash and comparison.
A call node is walked, copied, renumbered, hashed and compared as an
operator node is, with its function and its arguments where the operator
has its kind and its operands, and a copied `match` keeps its pointer to
the compiled constant pattern, which lives in the program arena the copy
outlives nothing of. A supplementary predicate's column that holds a
computed string holds it interned, when the supplementary tuple was stored.
The rewrite's limit counts sizes, and a call's size is its nodes.

---

## 11. Explain

A call prints as written, `fn(arg, ...)`, with each argument printed by the
rules of `specs/design-arithmetic.md` section 11. A call is a primary, so it
is never parenthesized, and an argument is a whole expression, so it never
needs parentheses at its top: `cat(substr(S, 0, length(S) - 2), ".o")`,
`-length(S)`, `index(P, "..") >= 0`. Parsing the printed text gives back
the tree that was printed, which the tests check. A string constant among
the arguments is escaped as a string constant in a comparison is today, so
every line is printable ASCII whatever bytes a pattern holds:

```text
rule untested(P) :- file(P), index(P, "src/") = 0, match(P, "\\.c$") >= 0, not tested(P).
    scan file(P)
    filter index(P, "src/") = 0
    filter match(P, "\\.c$") >= 0
    anti-member tested(P)
    emit untested(P)
rule object(M, O) :- source(M, S), O = cat(substr(S, 0, length(S) - 2), ".o").
    scan source(M, S)
    assign O = cat(substr(S, 0, length(S) - 2), ".o")
    emit object(M, O)
```

The two operations print as they print today; what a head column holds is
not a property of the plan's text.

---

## 12. Diagnostics

All follow architecture section 56: one line, lowercase, located at the
byte to look at.

Parse errors, one per statement:

```text
rules.dl:3:11: error: foo is not a function
rules.dl:5:9: error: substr takes 2 or 3 arguments, not 4
rules.dl:6:16: error: integer 42 cannot be an argument of length
rules.dl:8:40: error: expression nested more than 32 levels deep
rules.dl:9:14: error: expected ',' or ')', found '('
```

Check errors, one per offending literal:

```text
rules.dl:9:22: error: invalid regular expression: unmatched (
```

Evaluation errors, the first met in serial order, which fail the
evaluation:

```text
rules.dl:7:14: error: string too long in rule object/2
rules.dl:9:22: error: invalid regular expression in rule c_file/1: nothing to repeat
<query>:1:12: error: string too long in the query
```

An error in a rule the Magic rewrite generated names the program rule it
came from, through the origin every compiled rule records.

The note of verbosity level 2, once per rule, for the first comparison in
it that was false because of an operand's type, ordering, arithmetic or
function:

```text
rules.dl:4:22: note: operand of index is a symbol; the comparison is false
```

No new warning. A quoted pattern in a message is the constant as the
program wrote it, escaped as explain escapes it, and cut as every quoted
name in a message is; no message quotes a computed string, so none can
carry text a program built.

---

## 13. Limits and untrusted programs

| Limit | Value | Where enforced |
| --- | --- | --- |
| a computed string's length | 2^32 - 2 bytes | `lib/exec.c`, before the result is built |
| distinct byte strings | 2^30 - 1 | `lib/symbol.c`, at the commit's reservation |
| nesting of a pattern's parentheses | 32 | `lib/regex.c`, at the `(` that would exceed it |
| nesting of an expression, calls included | 32 | `lib/parse.c`, as today |
| the pool's storage and the string bits | `max_memory`, with everything else | `lib/symbol.c` and `lib/value.c`, before a growth |
| the text stack, the pending bytes, the pattern buffer and the matcher's work space | `max_memory`, with the evaluation's working state; round-scoped in every mode | `lib/exec.c`, before a growth; `lib/merge.c`, in the admission |

A program relic reads may have been written by someone who wants it to
misbehave, and strings give such a writer three new tools, each of which
this design takes away:

* **undefined behaviour.** Every byte a function reads is inside a range it
  was given, every position is clamped before it is used and every length
  is checked before a byte is copied, section 4.2; the pattern compiler's
  descent is bounded, the matcher does not recurse, and the string
  functions do not, so no pattern and no string overflows the C stack. A
  pattern of any bytes compiles to a program of known size or to one of
  seven errors, and a fuzz target says so, section 15;
* **unbounded memory.** A rule that computes a new string per derivation
  grows the pool, and the pool is charged to `max_memory`, section 9.1, as
  the text stack, the pending bytes, the pattern buffer and the matcher's
  work space are charged with the evaluation's working state, sections
  8.1, 8.3 and 8.4; a pattern in a column costs its worker up to about
  360 bytes of charged scratch per byte of pattern while it is matched,
  which the limit sees. Without a limit such a program runs until the
  string limit, the pool's count limit or exhaustion stops it, the
  position every unbounded program is in today;
* **unbounded time.** `match` is linear in the string times the pattern by
  construction, section 8.4, so no pattern is catastrophic; `index` is at
  worst the product of its operands' lengths, as `memmem(3)` is, and
  `cat` is linear in its result. A non-terminating program runs until a
  limit stops it, as today. No step or time limit exists, and this design
  adds none; `TODO.md` records the question for 1.0.

Nothing a function computes reaches a caller unformatted: a computed string
is a string value and prints as one, with the escaping every format gives a
string; a computed integer prints as one. No diagnostic quotes a computed
value.

`index` is implemented over `memmem(3)`, a BSD and glibc interface that
FreeBSD, macOS and glibc all provide, with a replacement in `compat/`
selected by `configure` for a platform without it, per the dependency
policy's second rule.

---

## 14. Interface

**The library's interface does not change.** No function, type, flag or
structure field is added: values are still symbols, strings and integers,
and a computed string in a row is a string whose bytes `relic_string()`
returns. What changes is text:

* `relic_config_t`'s description of `max_memory` names the symbol pool
  among what it counts;
* `RELIC_F_NOTE_MIXED`'s description names a function operand of the wrong
  type beside the arithmetic and ordering cases;
* `relic_materialize()` and `relic_query_run()` name a string too long and
  an invalid pattern as reasons to fail, beside an undefined arithmetic
  result, and `relic_query_run()` says that a query whose plan calls a
  function is gathered;
* `relic_stats_t`'s `string_values` is described as counting the strings
  evaluation computed as well as those the program wrote, and
  `storage_bytes` as including the pool.

The library's interface version is not bumped, since the binary interface
is unchanged.

**The tool does not change.** A string error is an ordinary error with a
nonzero exit, `-vv` reports the function note with the others, `-l memory=`
bounds the pool, and `-e` prints calls. The manual pages describe the six
functions, that they are over bytes, the position convention, the
totality of `substr`, the note for a wrong type, the two errors, the
dialect of section 4.5 with its escaping, and where termination now rests.

---

## 15. Tests

Every test is added with the change that needs it and is run against the
tree before it, per `CLAUDE.md`. Unit tests are in the C harness of
`tests/unit/`; language tests are `.dl` files with their expectations.

**Parser.** Every function at every arity, against the tree it must build;
a call as an operand of every operator and as an argument of a call; a call
at the start of a body literal followed by each comparator, by each
operator and by `mod`, and an atom at the start followed by `,`, `.`, `?`
and, inside an aggregate's body, `}`; an atom
with an operator argument and with a call argument, each reported at its
byte with the message of section 3.3; a call to a non-function, to a
function at the wrong arity, and to an aggregate name, each at the name;
a constant of the wrong type in each argument position of each function;
both nesting bounds met and exceeded by one with calls, `cat` nested 32
and 33 deep and `substr` nested in each argument position; a head and a
fact holding a call rejected; every v1 and arithmetic program of
`tests/lang/` producing the tree it produced.

**Functions.** Each function over the empty string, a one-byte string, a
string holding a NUL, bytes above 0x7f and a line feed; `substr` at every
combination of position and length in `{INT64_MIN, -1, 0, 1, length - 1,
length, length + 1, INT64_MAX}`, against a reference that keeps positions
by the definition of section 4.2; `index` of the empty needle, a needle
longer than the haystack, a needle at the start, the end and overlapping
itself; `str` of both extremes of `int64_t`, zero, a symbol and a string;
`cat` reaching exactly the length limit and one byte past it; a symbol
operand of each of the five typed functions yielding no value, and `str`
of every type yielding one; the stack need of every shape, `substr` in its
own third argument at height 32 needing 63 cells and no more. Run under
`make check-ubsan`, which must find nothing.

**Matcher.** Each construct of section 4.5 alone and combined; the seven
errors at their bytes; the leftmost rule on `abc|b` over `abc` (0, not 1),
on `a*` over `baa` (0, the empty match), on `^b` over `ab` (-1) and `b$`
over `ba` (-1); anchors inside a group and in a branch; a set with `]`
first, `-` first and last, a reversed range, and a negated set over every
byte; a NUL in the pattern and in the string; a pattern of 32 and 33
nested groups; `(a*)*b` and `(a|a)*b` over a hundred thousand `a`s
completing within a bound the test measures against the string's length;
a program's instruction count never exceeding `3 * length + 1` over a
million random patterns; compile, print nothing, and match, against a
reference that enumerates every start position and decides each with a
backtracking matcher over short strings, so the two agree on a million
random pattern and string pairs.

**Plan and executor.** A text slot bound and read by a lookup, a member
and an anti-member when its string is assigned and when it is not,
including when its bytes are in the pool with the string bit clear; the
text stack cut back correctly by a rule whose outer generator rebinds an
inner `cat` a thousand times, its top never growing, and by one whose
outer generator makes the outer string longer each time while an inner
text slot is bound below it; a filter's texts and a number assignment's
texts discarded; the four data-sized buffers released with their charge
at the end of a merged, a declined and an abandoned round on the serial
scratch and on every worker's, the charged total at each round's start
equal at one thread and at several, the merge's admission adding the
capacity for the largest size any task recorded and refusing a round
whose serial run would refuse an insertion, and the serial rerun of an
abandoned round admitted where a serial run is; a batch with pending
strings and pending integers
together,
measured, rewritten, committed and inserted, the identifiers the measure
promised being the ones the commit assigned, in all three cases of the
measure, and a computed string equal to the highest-numbered predicate's
name growing the string bits with no pool entry; the pool's capacity after
two batches of 64 new strings equal to its capacity after one round of 128
and after 128 single internings; the batch returned before a tuple whose
strings would not fit its pending bytes, that tuple written once on the
resumed `EMIT`, and a single tuple larger than an empty batch's room
growing the buffer once; a variable pattern's program run by every worker
of a round at once against a shared constant program; a batch the
memory limit refuses at the measure leaving the pool unchanged; the
string-too-long error and the invalid-pattern error crossing the boundary
with nothing inserted; a variable pattern compiled per binding and an
invalid one failing on the binding that supplies it.

**Language tests**, each a golden file:

```text
strings-syntax        every function, as operand, argument and at a literal's start
strings-binding       O = cat(...), N = length(S), in rules and queries
strings-substr        positions at and past both ends, negative, huge
strings-index         contains, not-contains, prefix and suffix as comparisons
strings-match         the dialect, anchors, escaping through the string literal
strings-match-bad     a constant pattern refused at load; a variable one failing at run
strings-str           str of each type; str(foo) = "foo"; str(N) beside a printed N
strings-mixed         a symbol operand filtered out, and its -vv note
strings-too-long      the error, no rows, nonzero exit
strings-negation      section 4.3's argument: an error, and no affirmative answer
strings-recursion     a bounded cat recursion, at one thread and with -X parallel
strings-explain       -e over section 11's program
strings-magic         a query binding a computed string argument through the rewrite
strings-gathered      a calling query gathered, the same query without the call streamed
strings-memory        -l memory= refusing the pool's growth
strings-atom-or-call  length/1 as a predicate and length(S) > 3 in one program
```

The memory-limit tests are recorded at totals that count the pool.

**Equivalences.** `tests/equiv/strategies.sh` runs the string corpus whose
every program succeeds under every strategy, and the answers, the tuple
totals and `string_values` agree byte for byte, with the function note
filtered from standard error as the others are. A program on which some
strategy meets an error is pinned per strategy by golden test, as
`arithmetic-strategy` is. The identity at several threads is asserted
after a successful run, after a run a tuple limit fails and after a run a
string error fails, `string_values` and `storage_bytes` included.

**Generated programs.** The generator of `specs/design-hardening.md` gains
calls, safe by construction and defined by construction: a call's string
arguments are variables an earlier positive literal binds or constants
from a small pool of short strings, its integer arguments small constants
or bound variables, its patterns drawn from a fixed list of valid ones,
and the strings a generated program states are short enough that no `cat`
of the depth it generates can reach the length limit. So no generated program meets an error, every strategy is compared
on every program, and a disagreement is a bug. The fuzz dictionary gains
the six names and the pattern metacharacters; the loader target exercises
the parse and check errors, the evaluator target the two evaluation
errors, and a new target feeds `lib/regex.c` arbitrary patterns and
strings, which must compile or refuse and must match or not, without a
crash, a hang or a read outside its inputs.

**Sanitizers.** `make check-asan` over everything touching the text stack,
the pending bytes, the candidate buffers, the map and the pool;
`make check-ubsan` over the functions and the matcher; `make check-tsan`
over the parallel identity tests.

---

## 16. Order of work

Each step lands with its tests and is measured against the one before it.

1. **Front end and program model** (sections 3, 5, 6 without the pattern,
   11's printer). Calls in the grammar, the atom-or-call literal, the
   parse checks, the call node with its height and size, the function
   table, the printer. Every existing test holds; the new parse and
   printer tests hold. No plan runs a call yet: the planner refuses a rule
   holding one with an internal diagnostic, so nothing is half-evaluated.
2. **Plan and executor, serial, without `match`** (sections 7, 8.1 to 8.3,
   8.5, 9.1, 9.3, 12, 14). Text slots, the text stack, the cells, the
   `CALL` step and the computed stack need, the five functions, the
   read-only probe, pending strings with measure, rewrite and commit on
   the serial path, the pool charged, calling queries gathered. The
   language and equivalence tests hold at one thread and under every
   strategy but `parallel`. Gate: `object/2` of section 2 over a million
   sources within twice the time of the same join with a constant in
   place of the `cat`.
3. **The matcher** (sections 4.5, 8.4, 6's constant pattern).
   `lib/regex.c`, its unit tests, its fuzz target, `match` with a constant
   and with a variable pattern, the check error at load. Gate: `match`
   over a million file names with a five-piece pattern within three times
   the time of `index` over the same names.
4. **Magic Sets and the frozen round** (sections 9.2, 10). Calls walked by
   the rewrite; pending strings in the candidate buffers, the screen by
   bytes, the measure and rewrite before the merge's first decline point,
   the commit in its reservation and append phases, the abandon on an
   error. Gate: the identity at every thread count, `string_values`
   included, and `object/2` scaling as `tc-cyclic` does at sixteen
   threads.
5. **Hardening and documentation** (sections 13, 15). The generator, the
   fuzz dictionary and corpus, the manual pages, a text workload in
   `bench/compare/` -- files grouped by extension and directory over a
   file-list data set -- measured against Souffle, which has `cat`,
   `substr` and `match`, and against DuckDB and SQLite, and
   `bench/baseline.txt` recorded again.

Steps 1 to 3 are the feature; 4 makes it complete under every mode the
tool has; 5 is what `CLAUDE.md` asks of every feature.

---

## 17. Determinism

* A function's value is a function of its operands' bytes and integers,
  computed without a locale, a character set or floating point, so it is
  the same on every platform.
* `match` is decided by relic's own matcher, so a pattern means the same
  everywhere relic runs; nothing depends on the platform's `<regex.h>`.
* A call is placed by the planner as a function of the source and the
  strategy, so which equalities bind, and where, is the same on every run.
* Every strategy that succeeds gives the same answer, section 4.3, and
  which strategies fail on a program is recorded by golden test.
* Strings computed by evaluation are interned in derivation order at their
  first stored occurrence, section 9.2, whether the round ran on one thread
  or many, so `relic_stats()` reports the same `string_values` at every
  thread count and identifiers are the same in every run; and, as always,
  nothing orders values by identifier.
* An evaluation error names the first error in serial order, at every
  thread count, because it is always the serial evaluator that meets it.
* `-e` prints a call from its tree, so equal trees print equal and the
  printed text parses back to the tree.

---

## 18. Amendments this design makes

* `specs/architecture.md` section 6: the six functions point here; section
  10.1: a function operand of the wrong type makes its comparison false,
  and a string too long or an invalid pattern fails the evaluation, section
  4.3; section 13: the `call` production and the atom-or-call literal of
  section 3; section 19.2: the pool is charged; section 63: the string
  length and the pool under the memory limit, and the two errors as
  ordinary errors; section 69: string operations point here; section 74:
  the matcher as an instance of the policy's fourth rule, and `memmem` of
  its second.
* `specs/decisions.md`: decision D-28 records that string operations are
  functions in expressions, that tests are comparisons over positions,
  that positions count from zero with -1 for none, that the matcher is
  relic's own with the dialect of section 4.5, and that a computed string
  rides on the pending-value mechanism of D-26 with the pool charged.
* `specs/design-parser.md` section 5: the call production and the
  atom-or-call literal; section 7: the call's diagnostics; section 9: a
  call counts toward both nesting bounds.
* `specs/design-program.md` section 8: the call node, section 6 here, and
  the compiled constant pattern.
* `specs/design-plan.md` section 3: the `TEXT_SLOT` and `CALL` steps and
  the computed stack need, section 7.4 here; section 4: the `TEXT` action.
* `specs/design-planner.md` section 5.1: the third slot kind, section 7.2
  here; section 8.2: calls in the printed text, section 11 here.
* `specs/design-eval.md` section 2.1: the `texts` array, the text stack,
  the three-kinded cell, the pending entries of two kinds and the pending
  bytes in the scratch, sections 8.1 to 8.3 here; section 2.3: comparisons
  over computed strings, section 4.4; section 7: a calling query is
  gathered, section 9.3; section 8: a string too long and an invalid
  pattern as ways an evaluation fails.
* `specs/design-relations.md` section 9: the pool is counted, section 9.1
  here.
* `specs/design-memory.md` section 7: the pool's growth to the stepwise
  capacity and its charge to the limit; section 8:
  `relic_value_find_string()`, the string bits' part in the measure and
  the commit and their reservation with the pool, sections 8.2 and 8.3
  here.
* `specs/design-comparison-cost.md` section 4.1: a plan with a call is
  gathered, section 9.3 here.
* `specs/design-parallel.md` section 4.4: a string error abandons a job;
  section 5.3: the screens for a pending string and the bytes in the
  candidate buffer; sections 6.1 and 6.3: the pool in the measure, the
  rewrite and the commit, section 9.2 here; section 7.1:
  `relic_value_find_string()` joins `lib/frozen.h`; section 6.1: the
  admission adds the serial scratch's capacity for the round's largest
  texts, pending bytes and patterns; section 8: the four data-sized
  buffers of every scratch are round-scoped and a worker's are released
  when its task finishes, section 8.1 here; section 12: `string_values`
  among the identities.
* `specs/design-magic.md` section 6.1: renumbering walks call nodes, and a
  copied `match` keeps its compiled pattern, section 10 here.
* `specs/design-hardening.md` section 2.4: calls generated safe and
  defined by construction, section 15 here; section 3.4: three failure
  kinds the driver names, the evaluation errors; section 5.1: the
  matcher's fuzz target.
* `specs/design-cli.md` section 9: the function note at level 2.
* `specs/design-api.md` sections 3, 5.1, 7 and 11: the text of section 14
  here.
* `docs/todo-language.md`: the string operations item points here.

---

## 19. What is deliberately absent

* **Builtin test literals.** `contains(S, T)`, `prefix(S, P)`, `match(S,
  R)` as body literals that `not` negates, Souffle's and Nemo's spelling.
  Each reserves a name at the start of a literal, where relic has let
  position decide; each is a second kind of literal in the compiled rule,
  the safety check, the planner, the executor, the rewrite, explain and
  the generator; and each is spelled by a comparison already, `index(S,
  T) >= 0`, whose negation is the opposite comparison. Section 2.
* **Reserved function names.** `length(X) :- ...` stays a rule and
  `length(a, 1).` a fact. Section 3.2.
* **A variadic `cat`.** Section 7.4: a stack cell per argument, and so a
  bound on the argument count or a stack sized per plan, for six
  characters of nesting.
* **Characters.** `length` counts bytes and positions are byte offsets,
  section 4.1. A function that counts UTF-8 code points, or cuts between
  them, is a later function with a defined answer for an invalid sequence;
  nothing here changes when it is added.
* **Conversions that can fail.** A string to the symbol it spells, which
  fails when the string is not `[a-z][A-Za-z0-9_]*`, and a string to the
  integer it spells, which fails on `"12x"` and on `"99999999999999999999"`.
  Each needs a no-value case a workload has not asked for; `str` is the
  conversion that cannot fail. Section 2.
* **Case conversion, trimming, splitting, replacing, `ord` and `chr`.**
  Each a few lines once the machinery exists; none needed by a workload
  this project measures. `upper` and `lower` would be ASCII-only by the
  rule of section 4.1 and should say so when added.
* **Solving for a variable inside a call.** `cat(X, "a") = "ba"` never
  binds `X`, as `X + 1 = Y` never does; `substr` is the inverse a program
  writes.
* **A match's extent.** `match` reports where the leftmost match begins
  and not where it ends, section 4.5, which is why the matcher needs no
  priority rule. A second function reporting the extent, or a `substr`
  form that takes a pattern, is the addition when a workload needs the
  matched text rather than the position; it brings POSIX's leftmost-longest
  rule with it.
* **Counted repetition, character classes, backreferences, captures,
  flags.** Section 4.5, each with its reason.
* **`<regex.h>`.** A libc interface, so no dependency; but glibc, macOS
  and FreeBSD each ship a different engine, and they disagree in edge cases
  of a standard that leaves them undefined, on bytes above 0x7f, and on
  what a NUL in the string does without `REG_STARTEND`; some are
  exponential on patterns an untrusted program can write; and every one
  is compiled under whatever locale the process has, which relic never
  sets. A `match` whose answer depended on the platform would break the
  determinism of architecture section 61 and the golden tests with it.
* **A vendored regex engine.** The dependency policy's third rule, for an
  engine that is a few hundred lines when written to the dialect relic
  needs and tens of thousands when taken whole; the audit cost is the
  reason to write it, per the fourth rule.
* **A cache of compiled variable patterns.** Section 8.4: a pattern in a
  column is compiled per binding. A small cache keyed on the pattern's
  identifier is the improvement, when a workload shows the compilation
  dominating.
* **String aggregates.** Concatenating a group into one string needs an
  order over the group that a set does not have, and
  `specs/design-aggregates.md` section 20 leaves it out with the set
  aggregates.
* **Reclaiming interned strings.** Section 9.1, with the integer case;
  D-35 says why.
* **Glob patterns.** `like` and `fnmatch`: a second pattern language for
  what `^` and `$` and `.*` say in the first.
