# relic floating-point design

**Status.** Design document for a floating-point value type: its syntax,
representation, equality, order, arithmetic, aggregates, functions, input,
output and determinism. It is recorded ahead of any implementation so that
the syntax it needs stays reserved and so that a language built as a
superset of relic's can add floats in the way relic will. Forward-looking,
like every document under `specs/`; `TODO.md` records what is not yet
implemented.

**Companions.** `specs/architecture.md` sections 3, 6, 10.1, 19 and 61;
`specs/design-parser.md` section 3.5 for the lexer's number rule;
`specs/design-arithmetic.md` sections 4 and 8 for the expression evaluator
and the pending value; `specs/design-comparison-cost.md` section 7 for the
domain identifier; `specs/design-aggregates.md` sections 4 and 9 for the
folds; `specs/design-strings.md` sections 3 and 5 for `str` and the
functions; `specs/design-import.md` section 4 for column types;
`specs/design-cli.md` sections 4, 5 and 9 for output; `specs/decisions.md`
D-3, D-4, D-11, D-24, D-26, D-27, D-28, D-30 and D-33.

---

## 1. What this document settles

* the literal syntax, which is the text D-30 reserves;
* that a float is a fourth type beside symbol, string and integer, how it
  is interned, and where its identifier's bits come from;
* that an integer never equals a float, and why the arithmetic design
  requires it;
* that an integer and a float order exactly, and what that costs;
* the arithmetic, and that a result that is not finite is an evaluation
  error;
* the aggregates `total` and `avg`, and why `sum` is not extended;
* the functions `str`, `float` and `int`;
* import, the output formats and the `-s` order;
* what makes the same program give the same bytes on every platform;
* what this design does not change.

Architecture section 3 keeps floats out of version 1 for two reasons: a
float would break the determinism of D-4 across platforms and the one
identifier per value of D-3. Sections 3 and 10 answer the first, section 3
the second. Neither objection is retired by hand-waving: each has a
mechanism here, and the mechanism is what the tests of section 12 check.

---

## 2. Syntax

A float literal is digits, a period, digits and an optional exponent:

```text
float := [0-9]+ "." [0-9]+ ([eE] [+-]? [0-9]+)?
```

There is no `.5`, no `5.`, no `1e5`, no hexadecimal form, and no spelling
of infinity or NaN. A leading `-` is the parser's, joined as it is for an
integer, D-16, so `-1.5` is a negative literal wherever a term may stand
and `X = -1.5` assigns the constant.

A literal whose nearest double is not finite is rejected where it is
written, as an integer out of range is:

```text
rules.dl:4:12: error: floating-point literal out of range: 1.0e400
```

Until the type exists, the lexer rejects the same text with
"floating-point numbers are not supported", pointing at the number's start,
D-30. That is what keeps every program relic accepts today meaning the
same thing once floats arrive: no accepted program contains the form. A
period directly after digits with no digit after it is still the
terminator, so `X = 1. 2 > 1?` is two statements under both rules. `1e5`
is an error under both as well: before the type exists it gets the same
message as `1.5`, and after it the lexer says what the form lacks, since
a literal without a period is not a float:

```text
rules.dl:4:12: error: a floating-point literal needs a period: 1e5
```

The import reader's `integer` column keeps its rule, `-?[0-9]+`, and a
`float` column reads the literal above with an optional sign, section 9.

---

## 3. A fourth type

A float is an IEEE 754 binary64 value, a `double`, and a type of its own:
`RELIC_FLOAT` in the public enumeration, with `v.real` in the value union.
It is as distinct from an integer as a string is from a symbol. The reasons
are in sections 4 and 5; the mechanism is this section's.

### 3.1 Interning

Floats are interned in a table of their own, a `double` array and a tagged
identifier table over it, built and charged to the memory limit exactly as
the integer table is, `specs/design-arithmetic.md` section 9.1. The key is
the value's bit pattern, which is what makes equality identity: two floats
get one identifier exactly when their bits agree.

Two patterns would break that, and both are kept out at the door:

* **Negative zero is normalized to zero** before interning, before
  comparison and before output, so `0.0` and `-0.0`, which IEEE 754 calls
  equal, have one identifier and print once. Nothing can observe the sign
  of a zero, because the one operation that distinguishes them, division,
  is an error on a zero divisor.
* **No NaN and no infinity exist in the language.** None can be written
  (section 2), imported (section 9) or computed (section 6). The table
  never holds one, so there is no value that is unequal to itself and no
  pair of infinities to order against the finite values. D-3's hazard, two
  encodings for one value, cannot return, because every value the table
  holds is a finite double with one bit pattern.

### 3.2 The identifier

D-24 gives a domain identifier two bits of type and thirty of index, with
1 for a symbol, 2 for a string and 3 for an integer. Type bits of zero are
not free: they are `RELIC_ID_NONE`, the identifier that is never a value,
which a zeroed slot or column reads as empty and a batch uses as the
pending marker of a computed value, `specs/design-arithmetic.md` section
8.3. A float cannot take them without making an empty column look like a
value.

**Tag 3 names a number, and the top bit of its index says which kind.**

```c
#define	RELIC_DOM_NUMBER	(UINT32_C(3) << 30)
#define	RELIC_DOM_FLOAT_BIT	(UINT32_C(1) << 29)
#define	RELIC_DOM_INTEGER	RELIC_DOM_NUMBER
#define	RELIC_DOM_FLOAT		(RELIC_DOM_NUMBER | RELIC_DOM_FLOAT_BIT)
#define	RELIC_DOM_NUMBER_MASK	(RELIC_DOM_FLOAT_BIT - 1)
```

An integer is `RELIC_DOM_INTEGER | k` for its position in the integer
table and a float `RELIC_DOM_FLOAT | k` for its position in the float
table. A context then holds at most 2^29 - 1 distinct integers and 2^29 - 1
distinct floats, and still 2^30 - 1 symbols. The two accessors that split
an identifier into type and index learn the third bit; nothing else reads
the bits, which is the point D-3 made of keeping the representation behind
six accessors. The symbol and string limits do not move, and the
zero-is-empty invariant does not move, which is why this is chosen over a
third type bit, which would halve every table, and over moving the pending
marker, which is read in four places.

A float's identifier hashes and compares as any word does; joins never
inspect it. Within the float table identifiers are assigned in order of
first appearance, like every other, and nothing may sort by them.

### 3.3 In the executor

A computed float lives where a computed integer lives: a slot of a fourth
kind, float, whose value is kept beside the frame and whose frame entry is
the identifier the table holds for it or zero when it holds none, found by
one read-only probe of the float table, `specs/design-arithmetic.md`
section 8.2. An `EMIT` of a float the table lacks writes a pending marker
and appends the value to a pending array of a third kind; the measure, the
rewrite and the commit of section 8.3 handle it as they handle an integer,
and a plan that streams holds no float slot, as it holds no number slot.

---

## 4. Equality is identity

**An integer never equals a float.** `1 = 1.0` is false, `1 != 1.0` is
true, and `p(X), q(X)` does not join a `1` of `p` with a `1.0` of `q`.
D-11 stands as written: equality is domain-identifier equality, and the
two values have different identifiers.

This is forced, not chosen. `specs/design-arithmetic.md` section 4.4 lets
an equality with one bound side bind the other, and section 4.5 proves
that every schedule the planner may choose produces the same bindings. The
proof needs `=` to be the identity of values: if `X = Y` could hold between
an integer and a float, then a schedule that bound `X` first would give `Y`
one value and a schedule that bound `Y` first would give `X` another, and
the two would derive different tuples. Clojure, whose `=` on numbers is
also exact across types, makes the same choice for the same reason, and
section 5 inherits its consequence.

A program that means numeric equality across the types writes it as two
orderings, `X <= Y, X >= Y`, or converts one side with `float()`, section
8. Under `-vv`, the note that today reports the first ordering between an
integer and a symbol in a rule reports an equality between an integer and a
float as well, worded for it:

```text
rules.dl:7:19: note: equality between an integer and a float is false
```

---

## 5. Ordering

Two floats order numerically. **An integer and a float order numerically
and exactly**, by the mathematical values, not by converting the integer
to a double first: `9007199254740993 > 9007199254740992.0` holds, where a
conversion would round the integer to the float and call them equal.

The comparison costs nothing a double comparison does not. For an integer
`i` and a finite float `d`: if `d >= 2^63`, `i < d`; if `d < -2^63`,
`i > d`; otherwise truncate `d` toward zero to an integer `t`, which is
exact and in range, and `i < d` exactly when `i < t`, or `i == t` and
`d > t`. The same three cases give `>`. There is no NaN to make the order
partial.

The accepted consequence, which section 4 forces: for `X = 1` and
`Y = 1.0`, `X <= Y` and `X >= Y` both hold while `X = Y` does not. A
comparison of a number with a symbol or a string stays false, and the
note of `specs/design-cli.md` section 9 reports it as it does now.

The total order that `-s` sorts by, which the language cannot observe,
changes in one way: numbers first, ordered by the rule above, an integer
before a float when they are numerically equal, then symbols, then strings
(section 9).

---

## 6. Arithmetic

The operators of `specs/design-arithmetic.md` section 4.1 extend to floats:

| Expression | Over two floats | Over an integer and a float |
| --- | --- | --- |
| `A + B`, `A - B`, `A * B` | the IEEE 754 result, round to nearest, ties to even | the integer is converted to the nearest double, then as two floats |
| `A / B` | true division | the same conversion, then true division |
| `-A` | the negation | -- |
| `A mod B` | no value | no value |

Two integers keep today's rules: `/` truncates and `mod` is defined.
`mod` is integer-only; a float operand gives the expression no value when
the operand is a variable and rejects the program when it is a constant,
exactly as a symbol operand does, section 3.5 of the arithmetic design.
The conversion of an integer operand rounds when its magnitude exceeds
2^53, and that rounding is the ordinary rounding of a float operation,
not a comparison: ordering is exact because an exact answer is free there,
and arithmetic converts because its result is a float in any case.

**A result that is not finite is an evaluation error**, in the class of an
integer overflow: an overflow to infinity, and a division by zero, which
includes `0.0 / 0.0`. The message is the one integers give, in the rule
that computed it:

```text
rules.dl:9:22: error: floating-point overflow in rule ratio/2
rules.dl:9:22: error: division by zero in rule ratio/2
```

The reason is section 4.2 of the arithmetic design, unchanged: a program
that meant a number and got none cannot have the binding quietly dropped,
because through negation a dropped binding derives an answer the program
denies. Underflow rounds, to a subnormal or to zero, and is not an error;
it is a defined result. No operation produces a NaN from finite operands
except `0.0 / 0.0` and `inf - inf`, and both are already errors by the rule
above, which is how section 3.1's promise is kept without a check per
result: the executor tests a result once, for finiteness, and a finite
result is never a NaN.

The expression depth limit and the postfix evaluator are unchanged; a cell
gains a float kind beside integer and identifier.

---

## 7. Aggregates

`count`, `min` and `max` are unchanged. `min` and `max` order within a
type, so a group mixing integers and floats has no value, as any group of
two types has none, `specs/design-aggregates.md` section 4.4, and the
`-vv` note reports it.

**`sum(V)` is unchanged and adds integers only.** A float row gives it no
value, as a symbol row does. It is not extended because an aggregate's
function fixes the type of its result, section 8.3 of the aggregates
design, and a `sum` that returned an integer over an empty or
integer-only group and a float otherwise would have a result type that
depended on the data.

**`total(V)` sums integers and floats in floating point.** Its result is
always a float, `0.0` over an empty group; it has no value when a row holds
a symbol or a string; and a total that is not finite is an evaluation
error. It makes the promise section 4.5 of the aggregates design makes for
`sum`: **the result is a function of the group's set of rows, whatever
order they are met in.** Floating-point addition is not associative, so a
running `double` cannot keep that promise; the fold therefore accumulates
exactly and rounds once:

* the accumulator is a fixed-point integer wide enough to hold any sum of
  doubles exactly: a signed two's-complement number whose lowest bit is
  the smallest subnormal's, 2^-1074, and whose span covers the largest
  finite double, 2098 bits, plus 64 bits of headroom for 2^64 addends,
  which no group can reach, and a sign: 2176 bits, 34 words of 64;
* adding a double is shifting its 53-bit significand to its exponent's
  position and adding with carry, 34 word operations at most and usually
  three; adding an integer row is adding the integer at position zero;
* at the end the accumulator is rounded once to the nearest double, ties
  to even, and a magnitude past the largest finite double is the overflow
  error.

The accumulator is a few hundred lines of integer arithmetic with no
dependency, which the dependency policy prefers to a library; it is
exact by construction, so the test of section 12 can compare it against
an arbitrary-precision sum rather than against another rounding. The
alternative, sorting the rows by magnitude and summing with compensation,
is faster per row, is not exact, and would have made "a function of the
set" a claim about an algorithm's error bound rather than a fact. A group
pays 272 bytes of accumulator while it is open, beside the 16 bytes a
`sum` pays.

**`avg(V)`** is `total(V)` divided by the group's count, as a float, and
has no value over an empty group. It rounds twice, once in the total and
once in the division, and is still a function of the set, since both
operands are.

`total` and `avg` are recognized by position, after the `=` of an equality
and before `{`, as the four names are; a predicate may be called `total`.

---

## 8. Functions

* **`str(F)`** is the shortest decimal that reads back to the same float:
  the fewest significant digits that round-trip, then the closest such
  string. It always holds a period and is always a literal of section 2,
  so `str` and the lexer agree byte for byte, as they do for integers. The
  plain form is used when the decimal exponent is between -5 and 16
  inclusive, which is where a reader counts zeros without losing track,
  and the exponent form `d.ddde[+-]dd` otherwise, with at least one digit
  after the period in both: `1.5`, `100.0`, `1.0e17`, `2.5e-7`. Zero is
  `0.0`.
* **`float(V)`** converts an integer to the nearest double, is the
  identity on a float, and has no value on a symbol or a string.
* **`int(V)`** truncates a float toward zero, is the identity on an
  integer, has no value on a symbol or a string, and is an evaluation
  error, reported as an overflow, when the truncated value is outside the
  64-bit integers.

D-28 said `str` is the one conversion because it is the one that cannot
fail. `float` cannot fail either. `int` can, and it fails the way an
overflow fails rather than having no value, because `int(X)` of a float
too large is not a type mismatch the program could mean: it is a number
the machine cannot hold, section 4.2 of the arithmetic design. D-33 amends
D-28 to say so.

---

## 9. Input and output

**Import.** A column type `float` reads the literal of section 2 with an
optional leading `-`, and rejects anything else, including a value that
rounds to infinity and the spellings of NaN and infinity, with the
reader's ordinary message:

```text
prices.csv:3:9: error: expected a float, found "1e5"
```

**Output.** `text`, `csv` and `tsv` write a float as `str` does. `json`
writes a number that always holds a period or an exponent, which is what
`str` produces, so a reader can tell `1.0` from `1`; the consumer that
reads JSON numbers as doubles reads every float exactly, and the note of
`specs/design-cli.md` section 4 about 64-bit integers is unchanged.

**`-s`.** Numbers first, ordered by section 5's rule with an integer before
a float that equals it, then symbols, then strings. The type rank of
`relic_value_compare()` gives integers and floats one rank and lets the
within-rank comparison decide, with the tie-break written out.

**Stats.** `-X stats` counts distinct floats beside distinct integers,
symbols and strings.

---

## 10. Determinism

D-4 asks that the same program give the same bytes on every platform. For
floats that is a property of the compiler and the library, and each part
of it is a check, not a hope:

* **No excess precision.** `configure` requires `FLT_EVAL_METHOD` to be 0
  and fails otherwise. On i386 that means SSE2 arithmetic, which
  `configure` asks for; the x87 unit, which evaluates in 80 bits and rounds
  when it pleases, is the platform `specs/design-planner.md` section 3
  already names as the reason an estimate uses no floating point.
* **No contraction.** The build passes `-ffp-contract=off`, so `a * b + c`
  is two roundings everywhere and never one fused multiply-add. A test
  computes an expression whose fused and unfused results differ and pins
  the unfused one.
* **No fast math.** The build never enables it, and a test that depends on
  a subnormal result and on the sign of zero before normalization would
  catch a toolchain that did.
* **relic's own parser and printer.** The correctly rounded conversion
  from decimal to double and the shortest round-trip conversion back are
  relic's, in `lib/float.c`, written over exact big-integer arithmetic:
  the decimal is compared exactly against the halfway point between the
  two candidate doubles, which is slow and plainly correct, and literals,
  imports and output rows are not where time goes. `strtod` and `printf`
  are not used for a value, since a C library's are correct on the major
  platforms and promised nowhere.
* **No libm.** The operations are the four of section 6 and the
  conversions of section 8, all of which IEEE 754 defines exactly. No
  `sqrt`, `pow`, `exp` or trigonometric function exists, since each
  varies between libraries in its last bit.

A golden test runs a program over every operation, aggregate and function
of this document and compares its bytes, and the CI matrix of
architecture section 68 runs it on every compiler and architecture it
has.

---

## 11. Unchanged

The planner, the Magic Sets rewrite, stratification and the expression
depth limit do not change: a float is a value in a column, joined by its
identifier like every value. The hash of a tuple folds the identifier as it
folds any word. The memory limit charges the float table as it charges the
integer table. The public `relic_value_t` gains an arm of its union and the
enumeration a value, which is an added interface, `specs/design-api.md`
section 9; a caller that switches on the type and does not know
`RELIC_FLOAT` sees a value it did not expect, as it would any added
enumerator.

---

## 12. Tests

* **Lexer and parser:** every form of section 2 accepted, every excluded
  form rejected with its message and position; a literal that rounds to
  infinity rejected; `-` joined; `X = 1. 2 > 1?` still two statements.
* **Interning:** `0.0` and `-0.0` one identifier; `1` and `1.0` two; the
  table charged to the limit; the capacity of 2^29 - 1 reported as the
  integer table's is.
* **Equality and order:** `1 = 1.0` false and both orderings true; the
  exact cases around 2^53 and 2^63; the `-vv` note.
* **Arithmetic:** each operator over each pair of types; overflow, zero
  divisor and `0.0 / 0.0` as errors; underflow as a value; `mod` with a
  float constant rejected and with a float variable valueless.
* **`total`:** the accumulator against an arbitrary-precision sum over
  random groups, including a group whose terms cancel and one that
  overflows only in a careless order; the same result under every order
  the random suite's strategies visit rows in; `0.0` over an empty group.
* **Conversions:** `str` round-trips every double the random suite draws;
  `int` at the edges of the 64-bit range.
* **Formats:** `json` output parses back to the same floats; `-s` order.
* **Determinism:** the golden program of section 10.

---

## 13. What is deliberately absent

* **A float that equals an integer.** Section 4: the assignment proof
  needs identity.
* **NaN and infinity.** Section 3.1: a value unequal to itself breaks
  interning, and an infinity is an overflow the language reports instead.
* **A `sum` over floats.** Section 7: a result type that depends on the
  data.
* **A float result from a running `double`.** Section 7: not a function of
  the set.
* **`float` and `int` that have no value on overflow.** Section 8.
* **Hexadecimal floats, `.5`, `5.`, `1e5`.** Section 2: one spelling,
  which `str` produces and the lexer reads.
* **Decimal or rational numbers.** A second non-integer type with
  different rounding; the brief for this design asked for IEEE 754 doubles
  because that is what the data a program imports was written with.
* **Any libm function.** Section 10.
