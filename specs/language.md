# The relic language

This is the normative specification of the language relic reads: what a
program may say, what it means, and which programs are refused. The manual
pages are short references to it. How the front end, the planner and the
evaluator implement each rule is in the design documents linked below;
nothing here depends on them.

Related: [architecture](architecture.md), [front end](design-frontend.md),
[expressions](design-expressions.md), [aggregates](design-aggregates.md),
[analysis](design-stratify.md), [planning](design-planning.md),
[evaluation](design-eval.md), [floating point](design-floats.md).

## Overview

relic reads Datalog in the conventional, Prolog-like spelling: facts, rules
whose bodies are conjunctions of literals, and queries.

```prolog
% Which tasks can start: every dependency is done.
depends(parser, lexer).
depends(planner, parser).
done(lexer).

task(T) :- depends(T, _).
task(T) :- depends(_, T).

ready(T) :-
    task(T),
    not done(T),
    N = count { depends(T, D), not done(D) },
    N = 0.

ready(T)?
```

The language is pure Datalog extended in directions that are well
understood and that real programs need: **stratified negation**;
**comparisons** over three value types, symbols, strings and 64-bit
integers; **integer arithmetic**, with an equality that binds a variable to
a computed value; **string functions** over bytes, with relic's own regular
expressions; and the **aggregates** `count`, `sum`, `min` and `max` over a
relation that is complete when it is read.

A few principles explain most of what follows.

* **Relations are sets.** A fact stated twice is one fact, an answer is a
  set of rows, and nothing in the language can observe a multiplicity.
* **Absence is meaningful because lower strata are complete.** Negation and
  aggregates read only relations that cannot grow any more, so "no such
  tuple" and "the count is 3" are facts, not guesses.
* **Position decides; no word is reserved.** `not`, `mod`, the aggregate
  names and the function names are recognized only where they can stand,
  and are ordinary symbols and predicate names everywhere else.
* **Bytes, never characters or locales.** The same bytes are the same
  program and the same answer on every platform and in every locale.
* **Columns are untyped, values are typed.** A column may hold values of
  several types, and a comparison across types has a defined answer.
* **No wrong answers.** A result the machine cannot represent stops the
  evaluation instead of being dropped, wrapped or saturated, since each of
  those can become an affirmative answer the program denies.
* **Every strategy gives the same answer.** The plan, the join order, the
  Magic Sets rewrite and the thread count never change what a program
  derives; they can change only whether an evaluation meets an error.

## Lexical structure

A program is a sequence of bytes. Classification uses relic's own table,
never `<ctype.h>` or the locale, so the same bytes are the same tokens
everywhere.

**Whitespace** -- space, tab, newline, carriage return, vertical tab and
form feed -- separates tokens. **`%` begins a comment** that runs to the end
of the line, wherever a token may begin; a comment on the last line needs
no newline, and there are no block comments.

### Names

A name is an ASCII letter or underscore followed by ASCII letters, digits
and underscores. Its first byte decides what it is:

| Token | Form | Example |
| --- | --- | --- |
| symbol | `[a-z][A-Za-z0-9_]*` | `edge`, `task_1`, `aB` |
| variable | `[A-Z_][A-Za-z0-9_]*` | `X`, `Node`, `_Unused` |
| anonymous variable | `_` alone | `_` |

A symbol token is a predicate name, a symbol constant, a function name, or
one of the [words recognized by position](#words-recognized-by-position).

### Integer literals

An integer literal is decimal digits whose value fits a signed 64-bit
integer. Leading zeros are accepted: `007` is 7 and `-0` is 0.

**The sign belongs to the parser, not to the lexer.** `-` is always a token
of its own, because `X-1` must be a subtraction, and the parser joins a `-`
followed by an integer into a negative literal wherever a term or an
operand may stand. So `offset(start, - 1).` is the same fact as
`offset(start, -1).`, `X - -1` is `X - (-1)`, and `X = -1` assigns the
constant -1 rather than negating 1. It is also what makes the least integer
writable: the lexer reads `9223372036854775808` as a magnitude and the
parser checks the range with the sign known, so `-9223372036854775808` is
accepted and `9223372036854775808` is refused, naming the literal.

A letter, digit or underscore immediately after the digits is an error at
that byte; otherwise `0x10` would read as `0` followed by the symbol `x10`,
and the complaint would be about a missing comma further on.

Digits followed by a period and a digit, or by an exponent (`e` or `E`, an
optional sign and a digit), are [reserved](#reserved-syntax) for
floating-point literals and refused as a whole, at the first digit, with
"floating-point numbers are not supported". A period after digits followed
by anything else is the statement terminator: `p(1).2 > 1?` is a fact and a
query.

### String literals

A string literal is written in double quotes and holds arbitrary bytes.
Bytes above 0x7f pass through unexamined, so UTF-8 text is held exactly
without relic knowing what UTF-8 is. The escapes are

```text
\\   \"   \n   \t   \r   \0   \xHH
```

where `\xHH` takes exactly two hexadecimal digits of either case; `\0` puts
a NUL byte in the string, which is an ordinary byte of it.

**An unknown escape is an error, not a literal character.** The permissive
reading cannot be taken back: once `"\q"` means `q`, any meaning given to
`\q` changes what existing programs say.

**A raw newline cannot appear in a string literal.** A literal that reaches
the end of its line is reported as unterminated, at its opening quote. A
missing closing quote is among the commonest typos, and allowing newlines
would let one swallow the rest of the file and be reported hundreds of
lines from the mistake. A newline in a string is written `\n`.

### Punctuation and operators

```text
:-                       separates a rule's head from its body
,  .  ?                  separate literals; end a fact or rule; end a query
(  )  {  }               arguments, calls and grouping; an aggregate's body
=  !=  <  <=  >  >=      comparators
+  -  *  /               arithmetic operators
```

Any other byte outside a string literal or a comment -- every byte above
0x7f and every control byte included -- is an error, `unexpected
character '$'` or `unexpected character 0xc3`. Identifiers are therefore
ASCII, which is what lets names relic generates, which contain `$`, never
collide with a name a program writes ([reserved syntax](#reserved-syntax)).

### Words recognized by position

| Word | Meaning | Where |
| --- | --- | --- |
| `not` | negation | where a body literal begins |
| `mod` | remainder operator | after an operand |
| `count`, `sum`, `min`, `max` | aggregate function | as a comparison's right operand, followed by `(` or `{`; valid only as `V = ...` |
| `length`, `cat`, `substr`, `index`, `match`, `str` | string function | where an operand stands, followed by `(` |

Everywhere else these are ordinary symbol tokens. `status(foo, not).`
stores the symbol `not`, `X = count` assigns the symbol `count`, and a
predicate may be called `mod`, `count` or `length`: `length(a, 4).` is a
fact of `length/2`. A reserved word would reject data a user could
reasonably have, and one token of lookahead always decides: a symbol never
otherwise follows an operand without a comma, and a symbol in operand
position is never otherwise followed by `(` or `{`.

`not` costs one thing: **no predicate may be called `not`.** A fact
beginning `not` reads as a negation, so such a predicate could never be
defined, and a reference to one, `not not p(X)`, would be a typo that every
later stage takes for an ordinary predicate with no rules. That reference is
refused with "not is the negation keyword and cannot name a predicate";
`not(a).` and `not :- q.` fail earlier, as a negation missing its atom
("expected a predicate name").

## Programs

A program is a sequence of statements: facts, rules and queries.

```prolog
edge(a, b).                                % a fact
path(X, Y) :- edge(X, Y).                  % a rule
path(X, Z) :- edge(X, Y), path(Y, Z).      % a recursive rule
path(a, X)?                                % a query
```

Several files, or several loads through the library, form one program.

**A predicate is its name and arity together**, written `name/arity`: `p/1`
and `p/2` are unrelated predicates that share a name, so there is no
arity-mismatch error. A predicate has at most 32 arguments. An atom with no
arguments is written `done` or `done()`, and names a relation of arity zero,
which holds the empty tuple or nothing. A predicate may be defined by facts,
by rules or by both; nothing declares a predicate extensional or
intensional.

**A fact** is a positive atom followed by `.`. It is a rule with an empty
body, so it must be safe: every argument of a fact is a constant, and
`edge(X, b).` is refused as an unsafe rule is. Stating a fact twice adds
nothing. Every rule, fact and query must be [safe](#safety) -- each variable
it reads gets a value from its body -- and the exact syntax of all three is
in [grammar](#grammar).

**A rule** is a head, `:-`, and a body of one or more literals separated by
commas, ending in `.`. The head is a positive atom whose arguments are
terms -- variables and constants, never expressions or aggregates. A body
literal is a positive atom, a negated atom ([negation](#negation)), a
comparison ([comparisons](#comparisons)), or an aggregate
([aggregates](#aggregates)). The body is a conjunction, and recursion,
mutual recursion included, is allowed subject to
[stratification](#stratification). Neither the order of a body's literals
nor the order of statements changes what a program means: the planner
chooses the order of evaluation, and every order gives the same answer.

**A query** is a body ending in `?`, holding any literal a rule body may:

```prolog
depends(X, Y), status(Y, todo)?
```

It is answered as a rule whose head names no predicate: the answer belongs
to the query, and no rule can read it. Its **output columns** are its named
variables that are not local to an aggregate, in order of first occurrence
anywhere in the query, inside braces included: a shared variable first
written inside an aggregate's braces takes its column there, so
`N = count { p(X) }, Y = 2, X = a?` has the columns `N`, `X`, `Y`. `_`
gives no column, while `_Y`, being named, does. A query has at most 32
output columns, and one with none answers one empty row when its body holds
and nothing when it does not. Answers are a set. Their order is unspecified
but the same on every run over the same input; `-s` sorts them by a total
order across types ([architecture: determinism](architecture.md#determinism)).

A query is answered against the whole program wherever it is written: a
fact stated after it is visible to it, and no query affects another. Query
text given apart from a program, through `relic -q` or the library, holds
exactly one query ending in `?` and no fact or rule, since preparing a query
must not change the program.

## Values and types

| Type | Written | Holds |
| --- | --- | --- |
| symbol | `foo`, `in_progress` | an identifier: a lowercase ASCII letter, then ASCII letters, digits and underscores |
| string | `"Parser cleanup"` | any sequence of bytes, NUL and bytes above 0x7f included |
| integer | `42`, `-7` | a signed 64-bit integer |

There are no floats, nulls, booleans, lists or structures; `true`, `false`
and `null` are symbols. A fourth type, an IEEE 754 double, is designed and
its syntax [reserved](#reserved-syntax).

**The types are distinct domains.** The symbol `foo` and the string `"foo"`
are different values, as are the integer `7` and the string `"7"`. A symbol
is spelled as an identifier wherever a value enters a program -- the text,
an imported table or the library -- so every symbol's spelling is one a
program could write.

**Equality is identity**: two values are equal exactly when they have the
same type and the same bytes or number.

**Columns are untyped.** A predicate declares no column types, and one
column may hold symbols in some tuples and integers in others. What a
comparison or a function does with an unexpected type is a defined answer,
given with each construct below.

**The language order** is defined within a type only: integers
numerically; symbols by spelling and strings by contents, bytewise as
unsigned bytes, a prefix before any longer value. Ordering is bytewise
rather than by locale collation so that no answer depends on the
environment. Values of different types are unordered; the total order `-s`
sorts answers by, integers before symbols before strings, is a presentation
order no program can observe.

## Variables

A variable's scope is one statement: `X` in one rule is unrelated to `X` in
another. Within a statement, every occurrence of one spelling is one
variable, except that a variable every occurrence of which lies inside one
aggregate's braces belongs to that aggregate ([aggregates](#aggregates)).

A variable gets its value from a positive atom that holds it, from an
[assignment](#assignment), or as an aggregate's result; every other
occurrence reads that value. [Safety](#safety) states the rule precisely.

**`_` alone is anonymous.** Every occurrence is a different variable that
no other occurrence can read, so it may stand only where no value need
exist: in an argument of a positive atom, where it matches anything and
binds nothing, and in an argument of a negated atom, where it is a column
the literal does not test. `_` in a head, in a comparison, as an
aggregate's result or as the variable of `sum`, `min` or `max` would be
read, and nothing can bind it, so each is refused.

**`_Name` is an ordinary named variable** that joins and binds like any
other. Its leading underscore says the writer ignores it on purpose, which
exempts it from the singleton warning; `p(X) :- e(X, _Y).` means the same as
`p(X) :- e(X, _).`. In a query, `_Y` is an output column.

**The singleton warning** reports a variable a rule mentions exactly once
whose spelling does not begin with `_`:

```text
project.dl:12:14: warning: variable Y occurs only once in this rule
project.dl:12:14: note: rename it to _Y to silence this warning
```

A singleton is the commonest Datalog typo: a misspelled variable that
silently joins with nothing. The warning changes nothing else. A variable
reported as unsafe is not also warned about, since the rename would
disguise the bug. A query's output variables are never singletons, because
the answer reads them, nor is a variable inside an aggregate's braces,
because it distinguishes the rows of the group. A variable an assignment or
an aggregate binds and nothing reads is one: a value computed and never
used.

## Comparisons

A comparison is a body literal `A op B` where `op` is one of
`=  !=  <  <=  >  >=` and each operand is a term or an
[expression](#expressions-and-assignment). Comparisons do not chain:
`1 < X < 3` is written `1 < X, X < 3`.

**`=` and `!=` are total.** Values of different types are never equal, so
`foo = "foo"` and `1 = "1"` are false and `foo != "foo"` is true.

**Ordering is defined within a type.** `<`, `<=`, `>` and `>=` compare two
integers numerically and two symbols or two strings bytewise. **An ordering
comparison between values of different types is false**, and the binding
that reached it is filtered out.

It is false rather than an error because columns are untyped: a column
holding a symbol in one tuple and an integer in another is a fact about the
data, not a mistake in the program, and an error would make whether a
program runs depend on which tuples exist. The answer given -- a symbol is
not greater than a hundred -- is honest and deterministic, and the `-vv`
note below keeps unintended mixing discoverable.

**A computed value compares as the value it is.** An expression's integer
equals an integer of that value and orders against any integer; a call's
string equals a string of those bytes and orders against any string;
against another type a computed value is unequal and unordered. So
`X + 0 = X` holds for every integer `X` and for no symbol, and
`length(S) > foo` is false and noted.

**An equality may bind.** When one side of `=` is a variable not yet bound
and the other side is fully bound, the equality is an
[assignment](#assignment) rather than a test.

### A value that does not exist

Several constructs can find, for a particular binding, that the value they
would compute does not exist: an ordering between two types, an arithmetic
operand that is a symbol or a string, a function argument of a type the
function does not take, and an aggregate over a group it has no value for.
The rule is the same for all of them:

> **The literal holding a value that does not exist is false, and the
> binding that reached it is dropped.** Verbosity level 2 (`-vv`, or the
> library's mixed-type note flag) reports the first such literal of each
> rule once, and of a query each time the query is answered.

```text
sizes.dl:4:22: note: ordering comparison between a symbol and an integer is false
sizes.dl:5:22: note: operand of + is a symbol; the comparison is false
```

This is a defined answer over untyped columns, not an error, and it is
distinct from an undefined result such as an overflow, which fails the
evaluation ([evaluation errors](#evaluation-errors)).

**Reversing a comparator therefore does not negate a comparison** when a
value may not exist. With `S` bound to a symbol, `length(S) > 3` and
`length(S) <= 3` are both false, and with `X` a symbol and `Y` an integer,
`X < Y` and `X >= Y` both are. The opposite comparator is the negation only
when both operands have values of one type, as when a positive literal
guarantees it. In general a test is negated through a helper predicate:

```prolog
long(S) :- name(S), length(S) > 3.
other(S) :- name(S), not long(S).      % every S that is not a long string
```

## Expressions and assignment

### Operators

An operand of a comparison may be an expression ([grammar](#grammar)):

| Operator | Meaning | Precedence | Associativity |
| --- | --- | --- | --- |
| `f(...)`, `( )` | call, grouping | highest | -- |
| `-A` | negation | | prefix |
| `*` `/` `mod` | product, quotient, remainder | | left |
| `+` `-` | sum, difference | lowest | left |

So `-X * 2` is `(-X) * 2` and `A - B - C` is `(A - B) - C`, the conventions
of C and of every Datalog with arithmetic. A `-` directly before an integer
is part of the [literal](#integer-literals); before anything else it is
negation, so `-X` and `-length(S)` negate.

**The remainder is spelled `mod`, not `%`.** `%` begins a comment wherever
a token may begin. Making it an operator inside expressions and a comment
elsewhere would need the parser to tell the lexer which it is in, and every
program with a comment after an expression would be one missing space away
from a different program. `mod` is recognized only after an operand, where
no symbol could otherwise stand, so it reserves nothing.

**Expressions stand only in comparisons.** A head, a fact and an atom's
arguments hold terms:

```prolog
succ(X + 1) :- n(X).          % refused
p(X) :- q(X, Y + 1).          % refused
succ(Y) :- n(X), Y = X + 1.   % accepted
```

An expression in a head would be shorthand for a fresh variable and an
assignment, which would have to be expanded somewhere: the rule relic runs
would hold a variable the writer never wrote, the plan `-e` prints would not
be the rule in the file, and the singleton warning and a query's output
columns would need rules for the unnamed variable. The assignment says the
same thing in the vocabulary the language has.

A **constant** operand of the wrong type is refused where it is written --
`symbol foo cannot be an operand of +` -- rather than making the
comparison false for every binding. A variable's type depends on the
tuples that bind it and is settled at evaluation.

An expression nests at most 32 levels, counting both the parentheses, call
parentheses and unary minuses open at once and the operators stacked on any
operand, where an operand is one level: a chain `X + 1 + ... + 1` joins at
most 32 operands. A deeper expression is split across assignments.

### Integer semantics

| Expression | Value | Defined when |
| --- | --- | --- |
| `A + B`, `A - B`, `A * B` | the exact sum, difference or product | the result fits 64 bits |
| `A / B` | the quotient, truncated toward zero | `B != 0` and the result fits |
| `A mod B` | `A - (A / B) * B`, with the sign of `A` | exactly when `A / B` is |
| `-A` | the negation | the result fits |

`-7 / 2` is -3, `-7 mod 2` is -1 and `7 mod -2` is 1: the rules of C, of
Souffle and of SQL. Tying the remainder to the quotient makes
`-9223372036854775808 mod -1` undefined exactly when the quotient is,
without a case of its own. An expression is evaluated for every binding
that reaches it, whether or not its operands are constants.

### Missing values and undefined results

Two different things can go wrong when an expression is evaluated, and they
are treated differently on purpose.

**An operand is a symbol or a string.** The expression has no value
([a value that does not exist](#a-value-that-does-not-exist)). In the
three-typed language `X = foo, X + 1 > 100` is false, and that is the
answer the program means. (`foo + 1 > 100`, with the constant written
under the operator, is refused when the program is read.)

**A result is undefined**: an overflow, a zero divisor, or the least
integer divided by -1. That has no answer the program means, and dropping
the binding can make a negation over it true, so it fails the evaluation
([evaluation errors](#evaluation-errors)):

```text
depth.dl:7:14: error: integer overflow in rule dist/3
budget.dl:12:9: error: division by zero in rule average/2
```

### Assignment

> **An equality one side of which is a bare variable not yet bound, and
> whose other side reads only bound variables, binds the variable to the
> other side's value.** Such an equality is an **assignment**; every other
> equality is a test.

A **bare variable** is a named variable alone, possibly in grouping
parentheses: `(X) = 1` assigns as `X = 1` does, while `X + 0 = 1` does not.

"Not yet bound" refers to the order the plan evaluates the body in. The
variable may stand on either side, and the value may be an expression, a
call, a constant or another variable:

```prolog
dist(X, Z, D1) :- dist(X, Y, D), edge(Y, Z), D < 8, D1 = D + 1.
next(Y)        :- n(X), X + 1 = Y.
answer(X)      :- X = 42.
copy(X)        :- r(Y), X = Y.
```

* **A constant binds**: `answer(X) :- X = 42.` is safe and derives
  `answer(42)`.
* **Both sides free binds nothing**: in `p(Y) :- X = Y.` both variables are
  unsafe.
* **Only a bare variable is assigned.** relic does not solve equations:
  `X + 1 = Y` binds `Y` when `X` is bound and nothing when only `Y` is, and
  `cat(X, "a") = "ba"` never binds `X`. Inverting `+` is easy, `*` and `/`
  are not exact, and inverting some operators and not others would be a
  list of special cases.
* **`_` is never assigned**: `_ = X + 1` is refused as unsafe.
* **Two bound sides make a test**: `p(X, N) :- q(X, N), N = X * 2.` keeps
  the tuples of `q` whose second column is twice the first.
* **A value that does not exist binds nothing**: the binding is dropped.

An aggregate literal, `N = count { ... }`, is the other form of assignment.

### Why the order of binding does not matter

Whether an equality assigns or tests depends on what is bound when the plan
reaches it, and the planner, the Magic Sets rewrite, the naive evaluator
and a parallel round each choose their own order. The answer does not
depend on the choice:

> Fix one tuple for each positive atom of a body. Every order the plan may
> choose either meets an undefined result, or produces exactly the bindings
> of the rule's variables that satisfy every comparison of the body, every
> expression having a value.
>
> *Every binding produced satisfies every comparison:* one evaluated as a
> test held; one evaluated as an assignment holds by construction, since it
> set its variable to make it hold; one over an expression with no value
> was false and dropped the binding. *Every satisfying binding is
> produced:* in it, an assigned variable has exactly the value its other
> side computes, because expressions are functions, so the assignment
> computes that value, and every test, evaluated over a satisfying binding,
> holds.

So every order that succeeds derives the same tuples, and they are the ones
the program derives over the mathematical integers, since a successful
evaluation computed every expression it needed exactly. Orders differ only
in whether one meets an undefined result another never reaches. The
argument needs `=` to be the identity of values, which is why a float
([floats](design-floats.md)) never equals an integer.

### Termination

A program whose rules compute nothing -- no arithmetic, no string function,
no aggregate -- terminates: every value it can store is in the program
before evaluation begins, so its relations range over a finite domain.
Arithmetic, string functions and `count` and `sum` create values. An
aggregate folds finitely many groups of a complete lower relation into one
value each, so it adds finitely many values per stratum, but recursion
through arithmetic or a string function need not end:

```prolog
n(0).
n(Y) :- n(X), Y = X + 1.       % derives until a limit or an overflow
```

so **termination is the program's responsibility**, as in every Datalog
with arithmetic. A recursive rule that computes should carry a bound, as
`D < 8` bounds `dist/3` above. The tool's part is the tuple and memory
limits, which stop such a program with a diagnostic
([architecture: limits](architecture.md#limits)).

## Functions

### Calls

A call, `name(argument, ...)`, stands wherever an operand stands. It binds
tighter than every operator, and its arguments are expressions, so calls
nest: `cat(substr(S, 0, length(S) - 2), ".o")`.

A body literal that begins `name(` is a call when an operator, `mod` or a
comparator follows its closing parenthesis, and an atom otherwise
([grammar](#grammar)): `length(S) > 3` holds a call, `length(a, 4).` is a
fact, and `p(X) :- length(X).` reads a predicate named `length`. `not`
takes an atom only; a test is negated as
[a value that does not exist](#a-value-that-does-not-exist) describes.

The parser refuses a call of a name that is no function, a wrong number of
arguments (`substr takes 2 or 3 arguments, not 4`), and a constant argument
of the wrong type (`integer 42 cannot be an argument of length`); an
aggregate name written as a call, `count(X)`, is a misplaced aggregate.

### The string functions

| Call | Arguments | Value | Result |
| --- | --- | --- | --- |
| `length(S)` | string | the number of bytes in `S` | integer |
| `cat(A, B)` | string, string | the bytes of `A` followed by those of `B` | string |
| `substr(S, I)` | string, integer | the bytes of `S` at positions `p` with `I <= p` | string |
| `substr(S, I, N)` | string, integer, integer | the bytes of `S` at positions `p` with `I <= p < I + N` | string |
| `index(S, T)` | string, string | the least `p` at which the bytes of `T` occur in `S`, or -1 | integer |
| `match(S, R)` | string, string | the least `p` at which the pattern `R` matches a prefix of `S` from `p`, or -1 | integer |
| `str(V)` | any value | a string itself, a symbol's spelling, an integer in decimal | string |

`substr` of two and of three arguments are two functions of one name, as
`p/2` and `p/3` are two predicates. The vocabulary is `awk`'s, whose model
of a string is relic's: bytes, and a question asked as a comparison over a
function's value.

**Bytes, not characters.** `length` counts bytes, positions are byte
offsets from 0, `substr` cuts between bytes and may split a multi-byte
UTF-8 sequence, and `index` and `match` compare bytes. Counting characters
would mean deciding what a character is -- a code point or a grapheme,
under which normalization, and what an invalid sequence counts as -- in an
engine whose determinism rests on never consulting a locale. Bytes have one
answer everywhere; `length` is what `wc -c` says.

**Every function is total over operands of its types.** `substr` is defined
by the positions it keeps, read over the mathematical integers, so every
integer argument is valid:

```text
substr("abc", -5, 100)   "abc"
substr("abc", -1, 2)     "a"      only position 0 lies in [-1, 1)
substr("abc", 5)         ""
substr("abc", 1, -1)     ""
```

A `substr` that failed out of range would turn every suffix test into two
literals. The empty string occurs at every position, so `index(S, "")` and
`match(S, "")` are 0, and `index(S, T)` with `T` longer than `S` is -1.
`str` of an integer is decimal with a leading `-` when negative and no
leading zeros, the text every output format prints, so `str(N) = "42"` and
a printed `42` agree byte for byte.

**Tests are comparisons** over a function's value:

```prolog
unsafe(P)    :- path(P), index(P, "..") >= 0.          % contains
safe(P)      :- path(P), index(P, "..") < 0.           % does not contain
under_src(P) :- path(P), index(P, "src/") = 0.         % begins with
c_file(P)    :- path(P), match(P, "\\.c$") >= 0.       % matches
object(S, O) :- source(S),
                substr(S, length(S) - 2) = ".c",        % ends with
                O = cat(substr(S, 0, length(S) - 2), ".o").
```

There are no builtin test literals such as `contains(S, T)`: each would
reserve a name at the start of a literal, where position decides, and
would be a second kind of literal everywhere in the engine, while the
comparison already binds, plans and prints. Over a column known to hold
strings, as here, the opposite comparison is the negation; over one that
may not, a test is negated through a helper predicate
([a value that does not exist](#a-value-that-does-not-exist)).

### Wrong types, long results and bad patterns

**An operand of the wrong type** -- a symbol or an integer where a string
is taken, anything but an integer where a position or a count is -- gives
the call no value ([a value that does not
exist](#a-value-that-does-not-exist)), noted at `-vv` as `operand of index
is a symbol; the comparison is false`. A symbol is not read as its
spelling, because a symbol and the string of its spelling are different
values: a function that blurred them would make `length(foo) =
length("foo")` hold where `foo = "foo"` does not. A program that means the
spelling writes `str(X)`. `str` takes every type, and is the one
conversion because it is the one that cannot fail; a string to the symbol
or the integer it spells can.

**A result longer than a string can be**, 2^32 - 2 bytes, fails the
evaluation as an overflow does (`string too long in rule object/2`); only
`cat` can produce one. **A pattern that is not a regular expression** is an
error ([regular expressions](#regular-expressions)). A recursive rule that
builds a longer string from one it derived, `s(Y) :- s(X), Y = cat(X, "a")`,
does not terminate on its own, for the reason arithmetic does not; bound
it, as `length(X) < 20` would.

## Regular expressions

`match(S, R)` reads `R` as a pattern of this dialect:

```text
pattern := branch ("|" branch)*
branch  := piece*
piece   := atom ("*" | "+" | "?")*
atom    := byte                 any byte but . [ ( ) | * + ? ^ $ \
         | "."                  any one byte
         | "[" set "]"          one byte of the set
         | "(" pattern ")"
         | "^"                  the start of the string
         | "$"                  the end of the string
         | "\" byte             that byte, whatever it is
set     := ["^"] item+          a leading "^" negates the set
item    := byte                 any byte but ] and \; a "]" first is a member
         | byte "-" byte        the bytes from the first to the second
         | "\" byte             that byte, whatever it is
```

Within a set, a range is formed only from an unescaped byte followed by `-`
and an endpoint that is neither `]` nor a backslash; an escaped byte always
stands alone. So `[a-]` holds `a` and `-`, `[\a-z]` holds `a`, `-` and `z`,
and `[]b]` holds `]` and `b`, since a `]` first is a member.

Alternation binds loosest, then concatenation, then the postfix
repetitions, which stack: `a**` is `a*`, and so is `a+?`. Every byte is
just a byte: `.` and a negated set match a newline and a NUL, and `^` and
`$` match only at the start and the end of the whole string, and `{` and
`}` are ordinary bytes.

**`match` reports the least position at which some match begins**, not the
extent of the match, so the dialect needs no rule choosing among matches
that begin at one position; `substr` cuts from the position.

**Backslashes are doubled in a literal.** A string literal's escapes are
decoded before the pattern is read, so the pattern `\.c$` is written
`"\\.c$"`, as in every language with string escapes, regular expressions
and no raw literal.

The grammar above is authoritative. It covers the common part of POSIX
extended regular expressions, where it agrees with `grep -E` under
`LC_ALL=C`, and leaves out: **counted repetition**, `a{3,5}`, which is
shorthand and the one construct under which a compiled pattern is not
linear in the written one; **character classes**, `[[:alpha:]]` and `\d`,
which name sets of characters where there are only bytes (`[a-zA-Z]` says
the same); **back-references**, which make matching NP-hard; and
**captures and flags**, since `match` reports a position and the engine
knows no case beyond ASCII. Where those constructs would be, the bytes
mean what the grammar says: `a{3}` matches the four bytes `a{3}`, and
`[[:alpha:]]` is a set of six bytes followed by a literal `]`.

**A match takes time linear in the string for a given pattern**; no pattern
makes it backtrack, so an untrusted program cannot write a catastrophic
one. relic matches with its own small engine rather than `<regex.h>`, whose
engines differ between platforms in edge cases and on bytes above 0x7f,
follow the locale, and are in some cases exponential; a platform-dependent
`match` would break determinism ([expressions](design-expressions.md)).

A pattern that is not a regular expression -- an unmatched `(` or `)`, an
unterminated `[`, a repetition with nothing before it (`*a`, `a|*`), a
reversed range (`[b-a]`), a trailing backslash, or parentheses nested more
than 32 deep -- is an error that says which. A pattern written as a string
literal is checked with the program, before anything is evaluated; any
other pattern, one read from a variable or computed as `cat("(", P)` is,
fails the evaluation that meets it:

```text
paths.dl:9:22: error: invalid regular expression: unmatched (
paths.dl:9:22: error: invalid regular expression in rule c_file/1: nothing to repeat
```

## Negation

A negated literal is `not` followed by an atom:

```prolog
ready(X) :- task(X), not blocked(X).
```

**It holds exactly when no tuple of the atom's relation agrees with it on
the columns that are not `_`.** A constant agrees with a column holding that
value, a named variable with a column holding its bound value, and `_` with
anything. Each `_` is quantified inside the negation:

```prolog
idle(C) :- customer(C), not order(_, C, _).
```

says "there is no order whose second column is `C`", not "some order's
second column is not `C`". It derives exactly what negating a helper
relation projected onto the named columns derives,

```prolog
order_of(C) :- order(_, C, _).
idle(C) :- customer(C), not order_of(C).
```

and spares the program that helper, once per use. Two consequences follow:

* `not p(_, _)` holds exactly when `p` is empty.
* **A named variable of a negated literal must be bound** by a positive atom
  or an assignment of the body. `not order(O, C, _)` with `O` bound nowhere
  is refused, not read as `not order(_, C, _)`: a variable named once is far
  more often a misspelling than an intended existential, and the language
  has one spelling for "any value here", `_`.

`not` negates an atom only; a test is negated through a helper predicate,
or by the opposite comparison where every value exists
([a value that does not exist](#a-value-that-does-not-exist)). A negated
literal reads its relation only when that relation is complete, which
[stratification](#stratification) guarantees and which is why a program
may not be recursive through negation. A predicate read under
`not` and never defined is empty, so the literal always holds, and the
undefined-predicate warning reports it ([diagnostics](#diagnostics)).

## Aggregates

### Syntax and position

```prolog
dep_count(T, N)   :- task(T), N = count { depends(T, D) }.
dir_size(Dir, S)  :- dir(Dir), S = sum(Size) { file(F, Dir), size(F, Size) }.
shortest(X, Y, M) :- node(X), node(Y), M = min(D) { dist(X, Y, D) }.
```

An aggregate literal is `V = count { body }` or `V = f(A) { body }` with
`f` one of `sum`, `min` and `max`. `V` is the **result**; `A`, the
**aggregated variable**, must be named, not `_`; `count` takes no variable,
since a set's size needs no column named. The **body** is a rule body of one
or more positive atoms, negated atoms, comparisons and assignments, and
**holds no aggregate**.

An aggregate stands **only as the whole right side of an equality whose
left side is a bare variable**:

```prolog
p(X, N) :- q(X), N = count { r(X, Y) }.                    % accepted
p(X) :- q(X), count { r(X, Y) } > 3.                        % refused
p(X, M) :- q(X), M = count { r(X, Y) } + 1.                 % refused
p(X, count { r(X, Y) }) :- q(X).                            % refused
p(X, N) :- q(X), N = count { r(X, Y), M = count { s(Y) } }. % refused
```

Each refused form is the accepted one plus a comparison or an assignment --
`N = count { ... }, N > 3`, or `M = N + 1` -- or, for nesting, an inner
aggregate written as a rule of its own whose relation the outer body reads.
An aggregate inside an expression would be an expression that reads a
relation, which no other expression does, and one in a head would be
shorthand for a variable the writer never wrote.

### Shared and local variables

> A variable every occurrence of which lies inside one aggregate's braces is
> **local** to that aggregate. Every other variable belongs to the rule, and
> one that also occurs inside the braces is **shared**.

The aggregated variable is an occurrence inside the braces and the result
an occurrence outside: in `S = sum(Size) { file(F, Dir), size(F, Size) }`,
`F` and `Size` are local and `Dir` is shared. A variable occurring in two
aggregates and nowhere else is shared with both, so that one spelling never
names two variables in a statement, and the rest of the rule must then bind
it. No grouping list is written: what groups the fold is what the rule has
already bound.

### The group

> For a binding of the shared variables, the **group** is the set of rows,
> over the named local variables, under which the body holds. The aggregate
> is a function of the shared binding and of the group, and of nothing
> else.

The shared binding matters only when the aggregated variable is itself
shared: each row then contributes the shared value, so over `p(1). p(2).`
the query `p(X), S = sum(X) { p(X) }?` answers `1 1` and `2 2`.

The group is exactly what the body would answer as a query with the shared
variables replaced by their values: `_` and constants contribute no column,
and two ways of satisfying the body that agree on every named local variable
are one row. Over `edge(a, b). edge(a, c).`:

```prolog
n(N) :- N = count { edge(a, Y) }.     % n(2): rows Y = b and Y = c
n(N) :- N = count { edge(a, _) }.     % n(1): one row, of no columns
```

The second is what `edge(a, _)?` answers, one empty row: an out-degree
counts distinct neighbours, and the neighbour is named. This is where the
set reading parts from SQL's `COUNT(*)`. Counting every way of satisfying
the body would make `sum(Y) { p(X, Y), r(X, _) }`
multiply each `Y` by the number of `r` tuples for `X`, a multiplicity no
query can show and no relation can hold; the set reading is stated in terms
the language already has.

### Aggregate functions

| Function | Value over a group | Over an empty group | Result |
| --- | --- | --- | --- |
| `count` | the number of rows | 0 | integer |
| `sum(A)` | the exact sum of the rows' `A` | 0 | integer |
| `min(A)` | the least `A`, in the language order | no value | a value of the group |
| `max(A)` | the greatest `A` | no value | a value of the group |

An aggregate has no value
([a value that does not exist](#a-value-that-does-not-exist)) in three
cases: `min` or `max` over an empty group; `sum` over a row whose `A` is a
symbol or a string; and `min` or `max` over values of more than one type,
which have no order. The last two are the aggregate form of a cross-type
comparison, and are noted at `-vv`:

```text
sizes.dl:4:22: note: a row of sum(Size) holds a value that is not an integer; the aggregate has no value
names.dl:7:19: note: the rows of min(N) hold values of more than one type; the aggregate has no value
```

Neither note names the row or the types it met, since which row a fold
meets first may differ between strategies; what is true of the whole group
is the same under every one.

`count` and `sum` are total, the empty group's being 0, which is what makes
`N = count { ... }, N = 0` hold for a task with no dependencies. A rule
that wants a default for `min` or `max` writes it:

```prolog
best(X, M) :- node(X), M = max(S) { score(X, S) }.
best(X, 0) :- node(X), N = count { score(X, S) }, N = 0.
```

**A sum is exact, and overflows as a function of its set of rows.** It is
accumulated without intermediate overflow and is an
[evaluation error](#evaluation-errors) only when the total over the
integers does not fit 64 bits. A group has no order the language can see,
and the order a fold visits rows in differs between strategies; a sum that
checked each addition would fail on `9223372036854775807, 1, -5` and
succeed on `1, -5, 9223372036854775807`, so whether a program failed would
depend on the strategy. Checking the exact total once makes the failure,
like the value, a function of the group. `count` cannot overflow, since a
group is a relation and holds far fewer than 2^63 rows.

### Binding and stratification

The aggregate literal is an assignment: when its result is not yet bound
and every shared variable is, it binds the result to the aggregate's value.
When the plan has already bound the result, by a positive atom, an
assignment or another aggregate, the literal tests the bound value against
the aggregate's: `p(X, N) :- q(X, N), N = count { r(X, Y) }.` keeps the `N`
of `q` that is the count. The argument that the [order of binding does not
matter](#why-the-order-of-binding-does-not-matter) covers aggregates as
well: the result is a function of the shared values and of a group that is
complete when the literal runs.

**A shared variable must be bound inside the braces as well as outside.**
The group is the body's own set of rows, so the body must be able to give
every shared variable its values:

```prolog
above(N) :- threshold(T), N = count { score(P, S), S > T }.                % refused
above(N) :- threshold(T), N = count { score(P, S), threshold(T), S > T }.  % accepted
```

```text
scores.dl:3:56: error: variable T in the aggregate of rule above/1 is not bound by a positive literal or an assignment of the aggregate body
scores.dl:3:56: note: a variable bound outside an aggregate must also be bound inside it
```

A result variable that also occurs inside its own braces,
`N = count { p(N) }`, is shared, so the aggregate cannot bind it: it is
unsafe unless another literal binds it, in which case the aggregate tests
that binding, as in `N = 1, N = count { p(N) }`.

**An aggregate may not read a relation that depends on the rule holding
it.** `shortest(X, Y, M) :- edge(X, Y), M = min(D) { shortest(X, Y, D) }.`
is refused: under a least fixed point the minimum over a relation that is
growing is not a fact about the relation, and the semantics that admit such
rules -- monotone aggregation over a lattice, or a well-founded reading --
are a second theory the language does not take on. Such a program is
written in two strata, a bounded recursive relation and the aggregate over
it once complete ([stratification](#stratification)).

### What the language leaves out

Each of these is spelled with what exists, or left for an extension that
changes no existing program's meaning:

* **a mean**: `S = sum(X) { ... }, N = count { ... }, N > 0, M = S / N`;
* **distinct values of one column**: name only it, `count { p(_, Y) }`;
* **a witness** of `min` or `max`: a second literal,
  `M = max(S) { score(X, S) }, score(X, M)`;
* **sorting and top-k**: `max`, negation and bounded recursion rank; `-s`
  orders output;
* **string and set aggregates**: they need an order over the group, which a
  set lacks, or a value type the language does not have;
* **glob and `like` patterns**: a second pattern language for what `^`, `$`
  and `.*` say;
* **case conversion, trimming, splitting**: small functions when needed;
  `upper` and `lower` would be ASCII-only, by the byte rule;
* **code-point functions**: separate functions with a defined answer for an
  invalid sequence, leaving `length` and `substr` as they are.

## Safety

Every rule, fact and query must be **safe**: every variable whose value is
read must get one from the body. This section is the one statement of the
rule.

Let *B*, the **bindable** variables, be the smallest set such that

1. every variable in an argument of a positive atom of the body is in *B*;
2. for each equality one side of which is a bare named variable *v* and
   whose other side's variables are all in *B*, *v* is in *B*;
3. for each aggregate whose result is a named variable and whose shared
   variables are all in *B*, its result is in *B*.

Let *R*, the **required** variables, be every variable occurring in the
head (for a query, every output column), in a negated atom other than as
`_`, in a comparison, or as an aggregate's result or shared variable.

> The statement is safe exactly when every variable of *R* is in *B*.

Each `_` in a head or a comparison is a distinct required variable that
nothing binds, so it is always reported. Each aggregate's body is checked
by the same rule as a rule whose head holds the body's shared and local
variables, which is why a shared variable must be bound inside the braces
too, and why the aggregated variable must be bound by the body. Its
offenders are reported at their occurrences inside the braces, the shared
variables first and then the local ones in order of first occurrence.

```prolog
ready(X) :- card(X), not blocked(X).           % safe
ready(X) :- not blocked(X).                    % X unsafe
foo(X) :- bar(Y).                              % X unsafe
big(X) :- size(X, N), N > 100.                 % safe
big(X) :- X > 100.                             % X unsafe
answer(X) :- X = 42.                           % safe: an assignment
p(Y) :- X = Y.                                 % X and Y unsafe
p(X) :- q(X), not r(X, Y).                     % Y unsafe
p(X) :- q(X), not r(X, _).                     % safe
c(T, N) :- task(T), N = count { dep(T, D) }.   % safe
p(_) :- q(X).                                  % _ unsafe
edge(X, b).                                    % X unsafe
```

**No ordering search is needed.** A positive atom needs nothing bound to be
scanned, so if any order exists in which every literal finds what it reads
bound, then taking every positive atom first, firing assignments and
aggregates as their inputs become bound, and placing the tests last is one.
Membership in *B* therefore decides whether a variable can be bound at all,
and every safe statement has a plan. Safety is one closure computation
rather than a search over the orders a body could run in, so the check
needs no planner and finds offenders in the order the text gives them.
What a reader can rely on: **in a safe
statement every variable is bound when it is read.**

**Every offender is reported**, each at its first occurrence, in the order
the variables first occur in the text, and checking continues with the
next statement. Each unsafe variable is an independent mistake, unlike a
cascade of parse errors after one bad token, so one run reports everything
wrong with a program:

```text
graph.dl:1:6: error: variable X in fact edge/2 is not bound by a positive body literal or an assignment
graph.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal or an assignment
graph.dl:3:5: error: anonymous variable in rule foo/1 cannot be bound by a positive body literal
```

The anonymous case has its own wording because the advice differs: a named
variable can be bound by adding a literal, and `_` never can.

## Stratification

The **dependency graph** has a vertex per predicate and an edge from a
rule's head to each predicate its body reads: a **positive** edge for a
positive atom, a **negative** edge for a negated atom, and an **aggregate**
edge for each predicate an aggregate's body reads. A comparison reads no
relation and makes no edge. A query is not in the graph; it is answered
once the whole program is complete.

> A program is **stratified** when no negative or aggregate edge lies on a
> cycle: no predicate depends negatively, or through an aggregate, on
> itself, directly or through other predicates.

A program that is not stratified is refused before anything is evaluated.

The predicates of a stratified program are assigned the least **strata**
such that every positive edge points to the same stratum or a lower one and
every negative and aggregate edge to a strictly lower one. Evaluation takes
one stratum at a time, from the lowest, each to its fixed point before the
next begins.

This is what gives negation and aggregates a meaning. A relation a negated
literal or an aggregate reads lies in a lower stratum and is complete when
it is read: no later derivation can add to it. "No such tuple" and "the
group has three rows" are therefore facts about the relation, not about how
far evaluation has got. In

```prolog
a(X) :- c(X), not b(X).
b(X) :- c(X), not a(X).
```

each predicate's absence would depend on the other's, and an aggregate
over a relation being derived would fold a value a later round
contradicts.

**The diagnostic names a cycle**, one note per edge, each at the literal
that makes it, since a bare "not stratified" leaves the reader to find the
cycle in a program large enough to have one by accident:

```text
policy.dl:18:1: error: program is not stratified
policy.dl:20:5: note: ready/1 depends negatively on blocked/1
policy.dl:25:5: note: blocked/1 depends on ready/1
```

Each offending group of mutually recursive predicates is reported once, through
one cycle: the first negative or aggregate edge inside it in written order,
then the shortest cycle through that edge, which need not be the shortest cycle
in the group. A cycle through an aggregate is described in the program's own
terms, at the literal inside the braces:

```text
paths.dl:9:1: error: program is not stratified
paths.dl:9:36: note: shortest/3 depends on dist/3 through an aggregate
paths.dl:12:5: note: dist/3 depends on shortest/3
```

How the graph is searched is in [the cycle
diagnostic](design-stratify.md#the-cycle-diagnostic).

## Meaning of a program

* **Each predicate denotes a relation**: a finite set of tuples of its
  arity, holding its facts and everything its rules derive.
* **A positive program means its least fixed point**, the smallest set of
  tuples containing every fact and closed under every rule.
* **A stratified program means its stratum-by-stratum least fixed point**:
  each stratum is the least fixed point of its rules over the complete
  relations of the strata below.
* **A negated literal** holds when no tuple of its relation, complete in a
  lower stratum, agrees with it on every column that is not `_`.
* **An aggregate** is a function of a complete relation of a strictly lower
  stratum, over the rows that agree with the values the rule has bound.
* **A comparison** holds or not by [comparisons](#comparisons); an
  expression or a call with no value makes it false.
* **A query's answer** is the set of bindings of its output columns that
  satisfy its body in that model.

Nothing else contributes. The order of statements, files and body
literals, the plan, whether a query is answered through the Magic Sets
rewrite or by whole-program evaluation, the thread count and every debugging
switch do not change the answer: every strategy that completes gives the
same set of rows. Strategies differ only in whether one meets an
[evaluation error](#evaluation-errors), or a resource limit, that another
does not. The order rows are written in is unspecified: it is the same on
every run over the same input, but `p(b). p(a). p(X)?` writes `b` before
`a`, and stating the facts the other way round reverses it; `-s` fixes
it. Answering a query never changes the program or what another query
answers; the values and predicates it interns persist for the life of the
context, so memory headroom and counters may depend on earlier runs.

## Evaluation errors

| Failure | Message |
| --- | --- |
| an integer result outside 64 bits, a `sum`'s total included | `integer overflow in rule p/N` |
| a quotient or remainder by zero | `division by zero in rule p/N` |
| a `cat` result longer than 2^32 - 2 bytes | `string too long in rule p/N` |
| a pattern that is not a regular expression and not a string literal | `invalid regular expression in rule p/N: ...` |

Each is located at the literal that met it and names the rule the program
wrote, or `the query`. **An evaluation error fails the evaluation**: the
query writes no rows, since a query whose plan can fail delivers none
before it finishes; the run exits with a nonzero status; and the next
evaluation starts over from the facts. An exceeded tuple or memory limit
fails an evaluation the same way, with a message such as
`relic: error: limit exceeded: tuples=N while deriving p/2`
([architecture: limits](architecture.md#limits)).

### Why an error and not a dropped binding

Dropping the binding, as a type mismatch does, looks conservative -- the
answer says less, never something false -- and is not, because the language
has negation. With `score(a, 9223372036854775807)`:

```prolog
blocked(X) :- score(X, S), S + 1 > 100.
allowed(X) :- score(X, _), not blocked(X).
```

dropping the overflowing binding leaves `blocked(a)` underived, and
`allowed(a)` is derived: an affirmative answer the program, read over the
integers, denies. A tool whose answers may decide what someone is allowed
to do cannot say `allowed(a)` because a number was too large. A type
mismatch has no such problem: a symbol score is not a lost integer, and
`blocked` over it is false in the three-typed language, which is what the
program means. An undefined result has no answer the program means, so the
only honest output is none. **Wrap-around and saturation** are worse:
they derive false tuples silently, with or without negation.

### What the error costs

Whether an evaluation meets an undefined result depends on which bindings
it evaluates expressions over, which depends on the strategy:

```prolog
n(0). n(1).
p(X, Y) :- n(X), Y = 1 / X.
p(1, Y)?
```

Through the Magic Sets rewrite the query evaluates only `X = 1` and
answers `1`; under whole-program evaluation (`relic -a`) the evaluation
also meets `1 / 0` and fails. **A strategy that evaluates more bindings
may fail where one that evaluates fewer succeeds**, and every strategy
that succeeds gives the same answer, the one over the integers. This is
the position SQL engines take -- a division by zero in a row a query never
needs is no error -- for the same reason: the alternative is a false
answer or a refusal to optimize. Deferring the failure to the end of
evaluation would not help, since strategies differ in which bindings they
evaluate at all.

## Reserved syntax

Three forms the front end refuses are reserved rather than merely absent, so
that an extension of relic, or a language built as a superset of it, can
give them a meaning without changing what any accepted program means:

* **Floating-point literals**: digits, a period and digits, with an
  optional exponent, as in `1.5` and `2.0e-3`; digits with an exponent
  alone, `1e5`, are refused with them, since their writer means a number. A
  float is designed as a fourth type ([floats](design-floats.md)): an IEEE
  754 double that equals only itself, so `1 = 1.0` is false, as the
  [order-independence argument](#why-the-order-of-binding-does-not-matter)
  requires, and that orders against an integer exactly by value, the one
  ordering across types that is defined.
* **`$` followed by a name**, for a parameter: a value supplied from
  outside the program. Generated names such as a query's `$q0` have the
  same shape, so a design for parameters keeps the two apart wherever both
  can appear, in a plan and in `-e` output: by substituting parameters
  before planning, or by printing a parameter in a form no generated name
  takes.
* **`#` followed by a name**, for a built-in constant or a directive. A
  file inclusion, if designed, is spelled `#include`. Program text is
  untrusted and may not open a path of its choosing, so an inclusion
  resolves a name relative to the including file, refuses `..` and
  absolute paths, and bounds its depth.

Nothing else is reserved: `true`, `false` and `null` are symbols, and no
word is a keyword. The names relic generates for its own relations
contain `$`, so no program can name, read or write one
([naming](architecture.md#naming)); they appear in `-e` plans, and
diagnostics name the program's own rules and predicates.

## Grammar

The complete grammar: `x*` is zero or more, `x+` one or more, `[x]`
optional, `|` alternatives, and terminals are quoted. `symbol`, `variable`,
`string` and `digits` are the tokens of [lexical
structure](#lexical-structure).

```text
program       := statement*

statement     := atom "."                        a fact
               | atom ":-" body "."              a rule
               | body "?"                        a query

body          := literal ("," literal)*

literal       := atom
               | "not" atom
               | comparison
               | aggregate

atom          := name [ "(" [ term ("," term)* ] ")" ]
name          := symbol                          other than "not"

term          := variable
               | symbol
               | string
               | integer

integer       := [ "-" ] digits

comparison    := expression comparator expression
comparator    := "=" | "!=" | "<" | "<=" | ">" | ">="

aggregate     := bare "=" function "{" group "}"
bare          := variable | "(" bare ")"
function      := "count"
               | ( "sum" | "min" | "max" ) "(" variable ")"
group         := group-literal ("," group-literal)*
group-literal := atom | "not" atom | comparison

expression    := product ( ( "+" | "-" ) product )*
product       := unary ( ( "*" | "/" | "mod" ) unary )*
unary         := "-" unary
               | primary
primary       := term
               | "(" expression ")"
               | call
call          := symbol "(" [ expression ("," expression)* ] ")"
```

The grammar's one ambiguity -- a literal beginning with a symbol -- is
resolved with one token of lookahead and no backtracking. Such a literal is:

1. a negated atom, when the symbol is `not`;
2. refused as a misplaced aggregate, when the symbol is an aggregate name
   followed by `{`;
3. when `(` follows, an atom or a call, decided by the token after the
   matching `)`: an operator, `mod` or a comparator makes the literal a
   comparison whose left expression begins with the call, and anything else
   makes it an atom, whose arguments must then be terms;
4. when a comparator or an operator follows, a comparison whose left
   expression begins with the symbol as a constant;
5. otherwise, an atom with no arguments.

A literal that begins with any other token that can begin a term, or with
`(`, is a comparison. What the notation leaves to prose:

* `integer` takes a `-` only directly before `digits`, space allowed
  between; elsewhere `-` is negation or subtraction.
* An aggregate is recognized as a comparison's right operand: an aggregate
  name followed by `(` or `{`. Unless the comparator is `=`, the left
  operand is a `bare` variable and no aggregate is already open, the
  literal is refused. The variable of `sum`, `min` and `max` is not `_`.
* A `call` names one of the [functions](#functions) with one of its
  arities.
* The [limits](#limits) apply, and every restriction on where variables
  may stand, a fact's included, is [safety](#safety)'s.

The front end's implementation, error recovery and locations are in
[front end](design-frontend.md).

## Diagnostics

A diagnostic about a program is one line, located at the byte the reader
should look at, as `file:line:column: error: message`, with `warning` or
`note` in place of `error`; a note adds to the diagnostic before it. How
messages are formatted and quoted is in [diagnostic
format](design-api.md#diagnostic-format).

The classes, in the order a run meets them:

* **Parse errors.** The grammar refuses a token or a statement, a literal is
  out of range, a constant has the wrong type, a grammar limit is exceeded.
  **A broken statement produces one message**, so the count of messages is
  the count of broken statements, and **a load with a parse error adds
  nothing** to the program: a program missing a rule is not a smaller
  program but a different one.
* **Check errors.** An unsafe variable, too many variables, an invalid
  constant pattern, a program not stratified. Every offender is reported,
  and nothing is evaluated.
* **Warnings**, which change nothing else: a singleton variable, and a
  predicate that is read and never defined, reported once, at the first
  atom that reads it. Such a predicate is far more often a typo than an
  intentionally empty relation, and its consequence is silent: every
  positive literal over it fails and every negated one holds.
* **Notes at verbosity level 2**: the first comparison, call or aggregate
  of each rule that had no value because of a type, once per rule, and in a
  query each time it is answered.
* **Evaluation errors**: the first failure an evaluation meets.

The error classes the library returns and the tool's exit status are in
[architecture: errors and exit status](architecture.md#errors-and-exit-status).

## Limits

The language's structural limits are fixed:

| Limit | Value | Why |
| --- | --- | --- |
| arguments of an atom | 32 | an index key is a 32-bit mask of columns |
| distinct named variables of a rule or query, not counting those local to an aggregate | 64 | a rule's bound variables are a 64-bit mask |
| distinct named variables inside an aggregate's braces, shared and local | 32 | the group is a relation of at most 32 columns |
| output columns of a query | 32 | each answer is a tuple of a relation |
| depth of an expression, in the text and in the tree | 32 | bounds the parser's recursion and every walk of the tree |
| nesting of a pattern's parentheses | 32 | bounds the pattern compiler's recursion |
| an aggregate inside an aggregate | refused | keeps the parser's descent one level deep |

Each is refused where the text exceeds it, naming what was exceeded, as in
`predicate p has more than 32 arguments`, `rule wide/1 uses more than 64
distinct variables`, `the query has more than 32 output columns`, `the
aggregate in rule wide/1 mentions more than 32 variables` and `expression
nested more than 32 levels deep`. A rule may hold twice as many variables as
a relation has columns, since most variables of a join are read rather than
stored, and variables local to an aggregate take none of the rule's 64, so
a rule with several aggregates over wide bodies does not reach the limit on
variables it never reads. `_` is no variable for either count, so a rule
may hold any number of them.

A string or a name holds at most 2^32 - 2 bytes, and a pattern is refused
when it is too long to compile, a bound far above any pattern a program
writes. A context's capacity for distinct values, predicates and tuples is
bounded as well; the distinct symbols and strings are counted by the byte
sequences behind them, so the symbol `foo` and the string `"foo"` take one
entry. Exceeding a capacity fails the load or the evaluation that needed
more ([architecture: limits](architecture.md#limits)).

Nothing else is bounded by the language: a program may hold any number of
statements, a body any number of literals, and an expression any number of
nodes within its depth. The resource limits on tuples, memory and the
Magic Sets rewrite's work belong to the tool and the library
([architecture: limits](architecture.md#limits)), and no limit bounds the
time an evaluation takes.
