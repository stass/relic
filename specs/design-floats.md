# Floating point

The design of a floating-point value type: its literal syntax, how a float
is interned and identified, equality and order against integers, arithmetic,
the aggregates and functions it adds, import and output, and what makes the
same program give the same bytes on every platform. A float is a fourth
type beside symbol, string and integer, an IEEE 754 binary64 value that is
always finite and never negative zero. Its literal form is reserved: a
relic without the type rejects that form, so no program such a relic accepts
means anything different where the type exists. The type is an extension of
the value model that reaches every stage that handles a value.

Related: [reserved syntax](language.md#reserved-syntax),
[values and types](language.md#values-and-types),
[comparisons](language.md#comparisons),
[expressions](language.md#expressions-and-assignment),
[evaluation errors](language.md#evaluation-errors),
[aggregates](language.md#aggregates), [functions](language.md#functions),
[determinism](architecture.md#determinism), [values](design-values.md),
[expressions](design-expressions.md), [aggregates](design-aggregates.md),
[tables](design-tables.md), [command line](design-cli.md),
[library interface](design-api.md).

## Model

Two objections stand against floats in a Datalog engine, and the design
answers each with a mechanism rather than an assurance.

* **Determinism.** relic promises the same bytes for the same program on
  every platform and at every thread count. Floating point is notorious for
  varying with the compiler, the C library, the hardware and per-thread
  state. The answer is to keep almost all float work off the floating-point
  unit -- comparison, conversion, summation, parsing and printing are integer
  arithmetic on bit patterns -- and to pin down exactly the environment in
  which the four arithmetic operators run (see [Determinism](#determinism)).
* **One identifier per value.** relic interns every value and compares by
  identifier, so a value with two encodings breaks joins and equality. IEEE
  754 has three hazards here: NaN, which is unequal to itself; infinities;
  and negative zero, equal to zero with a different bit pattern. The answer
  is one function through which every double must pass to become a float,
  which refuses what is not finite and folds negative zero into zero (see
  [Interning](#interning)).

A float enters from a literal, an imported column, an arithmetic result,
a fold, a conversion or a caller's `relic_value_t`, always through the one
function into the float table; from then on it is an identifier in a column,
and the planner, the Magic rewrite, stratification and storage do not
distinguish it.

## Invariants

* Every float value is a finite double other than negative zero. No NaN or
  infinity ever exists in the language.
* A float is equal only to itself: two floats are equal exactly when their
  bit patterns are, and an integer never equals a float.
* An integer and a float order exactly, by their mathematical values.
* A result that is not finite is an evaluation error, never a dropped
  binding.
* A fold over floats is a function of the group's set of rows, whatever
  order they are met in.
* Only `+`, `-`, `*` and `/` run on floating-point hardware, and always
  under the default environment.

## Syntax

A float literal is digits, a period, digits and an optional exponent:

```text
float := [0-9]+ "." [0-9]+ ([eE] [+-]? [0-9]+)?
```

There is no `.5`, no `5.`, no `1e5`, no hexadecimal form, and no spelling of
infinity or NaN. One spelling is what `str` produces and what the lexer
reads. A leading `-` is the parser's, joined to the literal as it is for an
integer, so `-1.5` is a negative literal wherever a term may stand and
`X = -1.5` assigns the constant.

A literal whose nearest double is not finite is rejected where it is
written, as an integer out of range is:

```text
rules.dl:4:12: error: floating-point literal out of range: 1.0e400
```

The form is reserved where the type is absent: the lexer rejects any text
of this shape with "floating-point numbers are not supported", pointing at
the number's start. Since no program accepted there contains the form, the
type gives no such program a new meaning. A period directly after digits
with no digit after it is the statement terminator either way, so
`X = 1. 2 > 1?` is two statements. `1e5` is an error either way; with the
type the lexer says what the form lacks, since a literal without a period
is not a float:

```text
rules.dl:4:12: error: a floating-point literal needs a period: 1e5
```

## Representation

A float is a `double`, `RELIC_FLOAT` in the public type enumeration, with a
`real` arm in the value union of `relic_value_t`. It is as distinct from an
integer as a string is from a symbol. `RELIC_FLOAT` is an added enumerator,
an interface addition by the versioning rules (see [C library and
ABI](architecture.md#c-library-and-abi)): a caller that switches on the type
and does not know it sees a value it did not expect, as with any added
enumerator.

### Interning

Floats are interned in a table of their own, an array of doubles with a
tagged identifier table over it, built and charged to the memory limit
exactly as the integer table is (see [values](design-values.md)). The key is
the value's bit pattern, which makes equality identity: two floats get one
identifier exactly when their bits agree.

Two patterns would break that, and the one function keeps both out:

* **Negative zero becomes zero.** `0.0` and `-0.0`, which IEEE 754 calls
  equal, get one identifier and print once. Nothing can observe the sign of
  a zero, because the one operation that distinguishes them, division, is
  an error on a zero divisor.
* **No NaN and no infinity.** The table never holds one, so there is no
  value unequal to itself and no infinity to order against finite values.
  Two encodings for one value cannot arise, because every value the table
  holds is a finite double with one bit pattern.

**One function makes a float.** A double becomes a relic float only through
one function, which refuses a value that is not finite and turns negative
zero into zero. It returns a type of its own, a struct around the double,
and the float table, the comparison, `int` and the formatter take that type
rather than a `double`, so a site that passes them a raw double does not
compile. The property holds by construction rather than by every call site
remembering a check. Every source reaches the table through it: a literal,
an import, an arithmetic result, the `total` fold, and a caller's value.

A caller supplies a float in a `relic_value_t`, and nothing stops that value
from holding a NaN, an infinity or a negative zero. `relic_fact_add()` and
every other entry point that takes a value pass it through the function: a
negative zero is stored as zero, and a value that is not finite is refused
with an error of the argument class. `relic_value_compare()` passes a float
through it too, so a negative zero compares as zero, but it has no failure
return and cannot refuse a value. It places a non-finite float after every
integer and every finite float, and orders two of them by their bit patterns
read as unsigned 64-bit integers. Its order stays total, so a caller who
sorts with it gets a meaningless position for a value relic never made, and
never undefined behaviour.

### The identifier

A domain identifier holds two bits of type and thirty of index: 1 for a
symbol, 2 for a string, 3 for an integer. Type bits of zero are not free:
they are the identifier that is never a value, which a zeroed slot or
column reads as empty and which marks a computed value pending interning.
A float cannot take them without making an empty column look like a value.

**Tag 3 names a number, and the top bit of its index says which kind.** An
integer is tag 3 with that bit clear and its position in the integer table; a
float is tag 3 with the bit set and its position in the float table. A context
then holds up to 2^29 - 1 distinct integers and as many distinct floats, while
symbols and strings keep their shared space of 2^30 - 1 (a string takes a
symbol identifier). Every type test goes through the accessors that split an
identifier into type and index, which learn the third bit; no other code reads
the type bits directly, so the change is confined to those accessors. This is
chosen over a third type bit, which would halve every table, and over moving
the pending marker, which several components read and which the zero-is-empty
invariant protects.

A float's identifier hashes and compares as any word does; joins never
inspect it. Within the float table identifiers are assigned in order of
first appearance, like every other, and nothing may sort by them.

### In the executor

A computed float lives where a computed integer lives: in a number slot of a
float kind, whose value is kept beside the frame and whose frame entry is
the identifier the table holds for it, or zero when the table holds none,
found by one read-only probe of the float table
([slots and cells](design-expressions.md#slots-and-cells)). An emitted
float the table lacks is written as a pending marker and its value appended
to a pending array; the measure, rewrite and commit of pending interning
handle it as they handle an integer or a computed string, so a
[shared round](design-parallel.md#model) never writes the context mid-plan
([interning computed values](design-values.md#interning-computed-values)). A
plan that streams holds no float slot, as it holds no number slot.

## Equality is identity

**An integer never equals a float.** `1 = 1.0` is false, `1 != 1.0` is
true, and `p(X), q(X)` does not join a `1` of `p` with a `1.0` of `q`.
Equality is domain-identifier equality, and the two values have different
identifiers.

This is forced, not chosen. An equality with one bound side binds the other
(see [expressions](language.md#expressions-and-assignment)), and the proof
that every schedule the planner may choose yields the same bindings needs
`=` to be the identity of values. If `X = Y` could hold between an integer
and a float, a schedule that bound `X` first would give `Y` one value and a
schedule that bound `Y` first would give `X` another, and the two would
derive different tuples. Clojure makes the same choice: `(= 1 1.0)` is
false, and numeric equality is a separate operator, `==`.

A program that means numeric equality across the types writes two
orderings, `X <= Y, X >= Y`, or converts one side with `float()`. Under
`-vv`, floats add an equality note for the integer and float pair, which no
other pair of types has, and exempt that pair from the note on orderings
between types, since an integer and a float do order:

```text
rules.dl:7:19: note: equality between an integer and a float is false
```

## Ordering

Two floats order numerically. **An integer and a float order numerically
and exactly**, by their mathematical values, not by converting the integer
to a double first: `9007199254740993 > 9007199254740992.0` holds, where a
conversion would round the integer and call them equal.

**Every comparison is integer arithmetic on bit patterns**, so it is exact
and the floating-point environment cannot reach it.

* Two floats compare by keys made from their patterns read as unsigned
  64-bit integers: a non-negative float's key is its pattern with the top
  bit set, a negative float's is its pattern with every bit flipped. Every
  negative key is then below every non-negative one, and within each sign
  keys order as values do. Negative zero would take a key of its own beside
  zero's; the one function has already removed it.
* An integer `i` and a float `d` compare by decomposing `d` into sign,
  exponent and significand. If `d >= 2^63`, `i < d`; if `d < -2^63`,
  `i > d`. Otherwise `d` truncated toward zero is an integer `t`, exact and
  in range, and `i < d` exactly when `i < t`, or `i == t` and `d` is
  positive with a nonzero fraction; `i > d` symmetrically. The magnitude of
  `t` is the significand shifted right by the exponent and compared as
  unsigned, since at `d = -2^63` it does not fit a signed integer. A shift
  of 64 or more, which every `d` below 2^-11 in magnitude needs, is not
  made: it gives `t = 0`, with a nonzero fraction exactly when `d` is not
  zero.

It costs a few shifts and compares, and there is no NaN to make the order
partial.

The accepted consequence, forced by equality: for `X = 1` and `Y = 1.0`,
`X <= Y` and `X >= Y` both hold while `X = Y` does not. A comparison of a
number with a symbol or a string stays false.

The total order that `-s` sorts by, which the language cannot observe,
places numbers by this rule (see [Input and output](#input-and-output)).

## Arithmetic

The arithmetic operators extend to floats:

| Expression | Two floats | An integer and a float |
| --- | --- | --- |
| `A + B`, `A - B`, `A * B` | the IEEE 754 result, round to nearest, ties to even | the integer converted to the nearest double, then as two floats |
| `A / B` | true division | the same conversion, then true division |
| `-A` | the negation | -- |
| `A mod B` | no value | no value |

Two integers keep the integer rules: `/` truncates and `mod` is defined.
`mod` is integer-only; a float operand gives the expression no value when
the operand is a variable and rejects the program when it is a constant,
exactly as a symbol operand does. Converting an integer operand rounds when
its magnitude exceeds 2^53, and that is the ordinary rounding of a float
operation, not a comparison: ordering is exact because an exact answer is
free there, and arithmetic converts because its result is a float anyway.

**A result that is not finite is an evaluation error**, in the class of an
integer overflow: an overflow to infinity, and a division by zero, which
includes `0.0 / 0.0`. The message is the one integers give, in the rule that
computed it:

```text
rules.dl:9:22: error: floating-point overflow in rule ratio/2
rules.dl:9:22: error: division by zero in rule ratio/2
```

The reason is the one for integers (see [evaluation
errors](language.md#evaluation-errors)): a program that meant a number and
got none cannot have the binding quietly dropped, because through negation a
dropped binding derives an answer the program denies. Underflow rounds, to a
subnormal or to zero, and is a defined result, not an error. From finite
operands, IEEE 754 produces a NaN only from `0.0 / 0.0` and `inf - inf`, and
both are already errors by the rule above. So the executor tests a result
once, for finiteness, and a finite result is never a NaN: the "no NaN"
invariant costs no check of its own.

The expression evaluator gains a float cell kind beside integer and
identifier; its depth bound and postfix form are those of every expression.

## Aggregates

`count` is type-blind. `min` and `max` order within a type, so a group
mixing integers and floats has no value, as any group of two types has
none, and the `-vv` note reports it. They do not take up the exact
cross-type ordering that `<` uses: `min` returns a value the group holds,
and over `1` and `1.0` both are least, so the answer would rest on a
tie-break between two values that are not equal, which only the `-s` order
has and the language cannot observe. A group of `1` and `2.5` has no such
tie, but giving a mixed group a value only when it has no tie would make
whether an answer exists depend on the data's ties, so every mixed group has
none.

**`sum(V)` adds integers only.** A float row gives it no value, as a symbol
row does. An aggregate's function fixes the type of its result, and a `sum`
that returned an integer over an empty or integer-only group and a float
otherwise would have a result type depending on the data.

**`total(V)` sums integers and floats in floating point.** Its result is
always a float, `0.0` over an empty group; it has no value when a row holds
a symbol or a string; and a total that is not finite is an evaluation error.
It keeps the promise every fold keeps: **the result is a function of the
group's set of rows, whatever order they are met in.** Floating-point
addition is not associative, so a running `double` cannot keep that
promise. The fold therefore accumulates exactly and rounds once:

* The accumulator is a fixed-point two's-complement integer wide enough to
  hold any sum of doubles exactly: its lowest bit is the smallest
  subnormal's, 2^-1074; its span covers the largest finite double, 2098
  bits; 64 bits of headroom allow 2^64 addends, which no group can reach;
  and a sign. That is 2176 bits, 34 words of 64.
* Adding a double shifts its 53-bit significand to its exponent's position
  and adds with carry, touching at most all words and usually three. Adding
  an integer row adds it with its units bit at the position of 2^0.
* At the end the accumulator is rounded once to the nearest double, ties to
  even, and a magnitude past the largest finite double is the overflow
  error.

The accumulator is a few hundred lines of dependency-free integer
arithmetic, which the [dependency policy](architecture.md#dependency-policy)
prefers to a library, and it is exact by construction, so it can be tested
against an arbitrary-precision sum rather than against another rounding.
Sorting the rows by magnitude and summing with compensation is faster per
row but not exact, and would make "a function of the set" a claim about an
algorithm's error bound rather than a fact. A group pays the accumulator's
272 bytes while it is open, beside the 16 a `sum` pays.

**`avg(V)`** is `total(V)` divided by the group's count, as a float, and has
no value over an empty group. It rounds twice, once in the total and once in
the division, and is still a function of the set, since both operands are.

`total` and `avg` are recognized by position, after the `=` of an equality
and before `{`, as the other aggregate names are; a predicate may be called
`total`.

## Functions

* **`str(F)`** is the shortest decimal that reads back to the same float:
  the fewest significant digits that round-trip, then the closest such
  string. It always holds a period and is always a literal of the syntax
  above, so `str` and the lexer agree byte for byte, as they do for
  integers. The plain form is used when the decimal exponent is between -5
  and 16 inclusive, where a reader counts zeros without losing track, and
  the exponent form otherwise: one digit, a period, the remaining digits,
  `e`, a `-` when the exponent is negative, and its digits without a
  leading zero. Both forms have at least one digit after the period:
  `1.5`, `100.0`, `1.0e17`, `2.5e-7`. Zero is `0.0`.
* **`float(V)`** converts an integer to the nearest double, is the identity
  on a float, and has no value on a symbol or a string.
* **`int(V)`** truncates a float toward zero, is the identity on an
  integer, has no value on a symbol or a string, and is an evaluation error,
  reported as an overflow, when the truncated value is outside the 64-bit
  integers.

`str` and `float` cannot fail. `int` can, and it fails as an overflow fails
rather than having no value, because `int(X)` of a float too large is not a
type mismatch the program could mean: it is a number the machine cannot
hold.

## Input and output

**Import.** A column type `float` reads the literal syntax with an optional
leading `-`, and rejects anything else, including a value that rounds to
infinity and the spellings of NaN and infinity, with the reader's ordinary
message:

```text
prices.csv:3:9: error: expected a float, found "1e5"
```

An `integer` column keeps its rule, `-?[0-9]+`.

**Output.** `text`, `csv` and `tsv` write a float as `str` does. `json`
writes a number that always holds a period or an exponent, which is what
`str` produces, so a reader can tell `1.0` from `1`, and a consumer that
reads JSON numbers as doubles reads every float exactly.

**`-s`.** Numbers first, by the exact order with an integer before a float
that equals it, then symbols, then strings. The shared total order behind
`relic_value_compare()` and `relic_result_compare()` gives integers and
floats one type rank and lets the within-rank comparison decide, with the
tie-break written out.

**Stats.** `-X stats` counts distinct floats beside distinct integers,
symbols and strings.

## Determinism

The same program gives the same bytes on every platform. For floats that is
a property of the compiler, the library and the thread the arithmetic runs
on, and each part of it is enforced, not hoped for. Most of the work never
reaches the floating-point unit: comparisons, the conversions, the `total`
fold, and relic's parser and printer are integer arithmetic on bit
patterns, exact under every compiler and environment. Only the four
operators use the hardware, and the first four items below are about them.

* **No excess precision.** `configure` requires `FLT_EVAL_METHOD` to be 0
  and fails otherwise. On i386 that means SSE2 arithmetic, which `configure`
  asks for; the x87 unit, which evaluates in 80 bits and rounds when it
  pleases, is also why planner estimates use no floating point at all.
* **No contraction.** The build passes `-ffp-contract=off`, so `a * b + c`
  is two roundings everywhere and never one fused multiply-add.
* **No fast math.** The build never enables it.
* **The floating-point environment.** The rounding mode and the flushing of
  subnormals to zero are per-thread state, in MXCSR on x86 and FPCR on
  AArch64. A caller may change the rounding mode with `fesetround()`, and
  loading a library built with fast math turns on flushing in the loading
  thread and every thread it creates afterwards. So every library operation
  saves the caller's environment on entry, where it lowers the interrupt
  flag, installs the default environment `FE_DFL_ENV`, and restores the
  caller's on return, exception flags included. Each job a pool worker runs
  installs the default as well, since a worker holds the environment its
  thread was created with, and a parallel round must compute what the
  serial one does (see [parallel evaluation](design-parallel.md)).

  Caller code that runs part way through an operation -- the diagnostic,
  explain and row callbacks and an import stream's read function -- runs in
  the default environment and must leave it as it found it. relic does not
  reinstall it after each call. A callback that changes it, itself or
  through a library it loads, changes the rest of the operation's arithmetic
  on the calling thread and not on the workers; `relic(3)` states the rule
  beside the other limits on what a callback may do.

  A self-test at entry refuses the operation on a platform where the default
  environment does not take: `fesetenv()` returned zero, `fegetround()` is
  `FE_TONEAREST`, the smallest normal double halved is not zero, and a
  subnormal doubled is not zero, each over `volatile` operands so the
  compiler cannot fold it. The refusal is of the internal class, since a
  library that cannot set its own arithmetic runs on a platform it does not
  support.
* **relic's own parser and printer.** The correctly rounded conversion from
  decimal to double and the shortest round-trip conversion back are relic's,
  in `lib/float.c`, written over exact big-integer arithmetic: the decimal is
  compared exactly against the halfway point between the two candidate
  doubles, which is slow and plainly correct, and literals, imports and
  output rows are not where time goes. `strtod` and `printf` are not used
  for a value: a C library's are correct on the major platforms and
  promised nowhere.

  "Slow" is bounded by construction rather than by the length of the text,
  since a literal may come from an untrusted program or table and the
  parser's arenas are outside the memory limit. A halfway point between two
  doubles has at most 768 significant digits, so digits past the 768th fold
  into one sticky digit, nonzero when any of them is: the input's first 768
  digits either differ from the halfway point's, which decides the
  comparison, or equal them, and then the input lies above it exactly when
  a later digit is nonzero. The exponent saturates at +-2^62 as it is read,
  past any count of digits a text can hold, so the decimal position of the
  leading digit -- the exponent adjusted by where that digit stands -- is
  exact or still beyond the range. A value whose leading digit stands at
  10^309 or above is out of range, and one below 10^-325 rounds to zero,
  both decided before any big-integer arithmetic. So the largest big
  integer a literal costs is a few kilobytes, whatever its length.
* **No libm.** The operations are the four operators and the conversions,
  all of which IEEE 754 defines exactly. No `sqrt`, `pow`, `exp` or
  trigonometric function exists, since each varies between libraries in its
  last bit.

A golden program over every float operation, aggregate and function runs on
every compiler and architecture of the [CI
matrix](design-release.md#continuous-integration) and is compared byte for
byte.

## Alternatives not taken

The rejected choices argued above -- a float equal to an integer, NaN and
infinity as values, a `sum` over floats, a running `double` fold, `int`
without a value on overflow, more than one spelling of a literal, a third
type bit -- are not repeated here. Two more:

* **Decimal or rational numbers.** A second non-integer type with different
  rounding; IEEE 754 doubles are what the data a program imports was written
  with.
* **`strtod`, `printf` and libm.** Correct on the major platforms and
  promised nowhere; relic's own conversions and the absence of transcendental
  functions keep every result defined by IEEE 754 alone.
