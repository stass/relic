# Language changes requested for relic

## Context

relic's language is the core of a larger query language in another project.
That language must stay a strict superset of relic's: every program relic
accepts is accepted there, with the same answers. It adds, in text relic
rejects today, float literals, `$name` parameters, `#name` constants and
named arguments. Nothing about that project needs to appear in relic.

The changes below do two things:

* remove the one place where the superset cannot be exact;
* record how relic would add floats, so that both stay one language.

They fall into three groups:

1. changes that must land **before 1.0**, because afterwards no accepted
   program may change meaning;
2. changes that only accept new text and can land **at any time**;
3. **floats**, recorded now as a future feature and not built.

Follow the working agreement for each: the spec first (design document and
a numbered decision), then the manual page, NEWS and tests. Section and
file references below are pointers to check, not verified facts. Where this
brief conflicts with an existing decision, stop and ask.

---

## 1. Before 1.0

### 1.1 Reject a period directly between two digits

Today `1.2` lexes as an integer, a period and an integer. So

```prolog
q(X) :- p(X), X = 1.2 > 1?
```

is the rule `q(X) :- p(X), X = 1.` followed by the query `2 > 1?`.

**Change.** An integer immediately followed by a period and a decimal digit
is a lexical error, reported at the start of the number, with a message
such as `floating-point numbers are not supported`. A space, a newline or a
comment after the period keeps today's meaning, so `X = 1. 2 > 1?` is still
two statements. `p(1).2 > 1?` is unaffected, since a `)` precedes the
period.

**Why.** The text is almost certainly a float written by someone who
expects one, and today it silently means something else. Rejecting it also
keeps `digits.digits` free, so floats can be added after 1.0 without
changing the meaning of any accepted program.

**Scope.**

* The lexer only. Check that the import reader already rejects `1.2` in an
  `integer` column and leave it alone.
* `1e5` is already an error (an identifier byte after the digits). Keep it
  one, and consider giving it the same message.

**Touches.** The lexer; the token table of `specs/design-parser.md`
section 3.2; the sentence on integers in `relic(1)`; language tests for a
fact, a rule body, a query and an atom argument; the random program
generator, which must never emit the adjacent form.

### 1.2 Record `#` and `$` as reserved for extension

Both bytes are already rejected outside a string literal. No behaviour
changes. Record as a decision what each is kept for:

* `$name` is a value supplied from outside the program, a parameter.
  Generated names already use `$` (the `$q` of `-e` output), so note that a
  parameter syntax must stay clear of them.
* `#name` is a built-in constant or a directive. If the file inclusion of
  `docs/todo-language.md` is designed, spell it `#include`.

Add tests that pin both bytes as errors.

---

## 2. At any time

Each of these accepts text that is rejected today or adds an interface, so
none changes an existing program.

### 2.1 Allow `_` in a negated atom

Today `_` may stand only in a positive body atom. Allow it in a negated
atom as well:

```prolog
idle(C) :- customer(C), not order(_, C, _).
```

**Meaning.** `not p(X, _)` holds when no tuple of `p` has `X` in its first
position. Each `_` is quantified inside the negation. The literal is
equivalent to negating a helper relation:

```prolog
p1(X) :- p(X, _).
... not p1(X) ...
```

That equivalence is a ready differential property for the random suite.

**Rules.**

* Named variables of a negated atom must still be bound by a positive atom
  or an assignment. `_` is exempt.
* `not p(_, _)` is allowed and tests that `p` is empty.
* `_` in a rule's head stays rejected.
* The same holds for a negated atom inside an aggregate's braces.
* Stratification is unchanged: the literal is a negative dependency on `p`.
* Check that the Magic Sets rewrite needs no special case: an anonymous
  position is a column the literal ignores.

**Execution.** An existence probe keyed on the columns that are not `_`,
which is an ordinary secondary index built on demand.

**Why.** A relation with many columns is otherwise unusable under `not`
without a helper rule per use.

### 2.2 An error class a caller can read

Already in `TODO.md`: "A caller tells a limit's refusal from another
failure only by its text." Give the library an error class for the last
failure, at least: program or query rejected, evaluation error (overflow,
division by zero, string too long, bad pattern), limit refused, I/O, out of
memory. A program that drives relic as a reference implementation needs to
tell these apart without matching message text.

### 2.3 Warnings as errors (optional)

A command-line flag and a library flag under which a check warning — a
variable named once, a predicate read and never defined — rejects the
program. Decide whether the warning for an abandoned Magic rewrite counts;
it reports a budget, not a mistake in the program.

### 2.4 A bound on work (optional)

Already in `TODO.md`: "No limit bounds the work of a join." If it is
built, prefer a deterministic count of work over wall-clock time, in
keeping with D-4.

### 2.5 Mark the language-only tests (optional)

Most programs under `tests/lang` carry an `.args` file, so many test the
tool rather than the language. A list of the tests whose expectation
depends only on the language — a program, then its answers or its
rejection — lets another implementation of the language run them as a
conformance suite. A manifest file or a naming convention would do.

---

## 3. Floats: a future feature, recorded now

Do not build this. Record the decisions below where relic keeps such
things — a `specs/design-floats.md` stub, the entry in
`docs/todo-language.md`, the "Floating point" item of
`specs/design-arithmetic.md` section 19, and the non-goal text of
`specs/architecture.md` section 3, which says a float would break D-3 and
D-4. The decisions answer both objections.

### 3.1 Syntax

* A float literal is digits, a period, digits and an optional exponent:
  `[0-9]+ "." [0-9]+ ([eE] [+-]? [0-9]+)?`.
* There is no `.5`, no `5.`, no `1e5`, no hexadecimal form, and no spelling
  of infinity or NaN.
* A leading `-` is joined by the parser, as for integers.
* A literal that is not finite once rounded is rejected.
* Change 1.1 is what reserves this syntax.

### 3.2 A fourth type

* A float is an IEEE 754 double and a type of its own, beside symbol,
  string and integer.
* It takes the fourth and last type tag of D-24. Record that no tag is left
  after it.
* Floats are interned by bit pattern in a table of their own, charged to
  the memory limit as the integer table is.
* Negative zero is normalized to zero before interning, so one value has
  one identifier (D-3).
* **No NaN and no infinity exist in the language.** None can be written,
  imported or computed.

### 3.3 Equality is identity

* An integer never equals a float. `1 = 1.0` is false, and `p(X), q(X)`
  does not join `1` with `1.0`.
* The reason is `specs/design-arithmetic.md` sections 4.4 and 4.5. An
  equality may be planned as an assignment, and the proof that the order of
  binding does not matter needs `=` to hold only between identical values.
* `-vv` notes an equality between an integer and a float, as it notes a
  cross-type ordering today.

### 3.4 Ordering

* Two floats order numerically.
* An integer and a float order **numerically and exactly**, not by
  converting the integer to a double: 9007199254740993 is greater than
  9007199254740992.0.
* Consequence, accepted: for `1` and `1.0`, `X <= Y` and `X >= Y` both hold
  while `X = Y` does not. Clojure has the same rule.
* Ordering a number against a symbol or a string stays false.

### 3.5 Arithmetic

* `+`, `-`, `*`, `/` and unary `-` on floats round to nearest, ties to
  even. `/` on two floats is true division; on two integers it truncates,
  as today.
* An integer operand beside a float one is converted to the nearest float
  first.
* `mod` stays integer-only. A float operand gives no value when it is a
  variable and rejects the program when it is a constant, as a symbol
  operand does today.
* **A result that is not finite is an evaluation error**, in the class of
  integer overflow: overflow, and division by zero, which includes
  `0.0 / 0.0`. The reason is the one section 4.2 gives for integers: a
  quietly wrong value derives, through negation, an answer the program
  denies. Underflow rounds and is not an error.

### 3.6 Aggregates

* `sum(V)` is unchanged and adds integers only. A float row gives it no
  value, as a symbol row does today.
* A new aggregate `total(V)` sums integers and floats in floating point:
  * its result is always a float, and 0.0 over an empty group;
  * it has no value when a row holds a symbol or a string;
  * a total that is not finite is an evaluation error;
  * it is a function of the group's set of rows, whatever order they are
    met in — the promise `specs/design-aggregates.md` section 4.5 makes for
    `sum`. That needs exact accumulation with one final rounding; the
    design picks the algorithm.
* The function fixes the type of the result. That is why `total` is
  separate: a `sum` over floats would be an integer 0 for an empty group
  and a float otherwise.
* `min` and `max` are unchanged: a group mixing integers and floats has no
  value, as any group of two types has none.
* `count` is unchanged.
* Optionally `avg(V)`: the mean as a float, with no value over an empty
  group.
* `total` and `avg` are recognized by position, as the four names are.
  Nothing becomes reserved.

### 3.7 Functions

* `str(F)` is the shortest decimal that reads back to the same float. It
  always holds a period, and is always a valid float literal. The design
  fixes when the exponent form is used.
* `float(V)` converts an integer to the nearest float.
* `int(V)` truncates a float toward zero, and is an evaluation error
  outside the 64-bit integers.

### 3.8 Input and output

* An import column type `float` reads the literal syntax with an optional
  sign and rejects anything that is not finite.
* `text`, `csv` and `tsv` write a float as `str` does. `json` writes a
  number that always holds a period or an exponent, so a reader can tell a
  float from an integer.
* `-s` needs a place for floats. Suggested: numbers first, ordered
  numerically, an integer before a float on a tie; then symbols; then
  strings.

### 3.9 Determinism

The same program gives the same bytes on every platform (D-4):

* no fused multiply-add contraction, no excess precision (require
  `FLT_EVAL_METHOD` 0), no fast-math;
* relic's own correctly rounded parser and printer, not libc's;
* no libm function whose result varies between platforms.

### 3.10 Unchanged

The planner, the Magic Sets rewrite, stratification and the expression
depth limit. A computed float is pending and interned as a computed
integer is.

---

## 4. Not requested

No other language change. In particular `true`, `false` and `null` stay
ordinary symbols, columns stay untyped, and nothing becomes a reserved
word.
