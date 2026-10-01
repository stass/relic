# relic arithmetic design

**Status.** Design document for arithmetic expressions: integer expressions
in comparisons, the equality that binds a variable to one, and what a value
evaluation computes rather than reads does to the value table, the executor
and a parallel round. Forward-looking, like every document under `specs/`;
`TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 6, 10, 13, 14, 19, 25, 61,
63, 66 and 69; `specs/design-parser.md` for the front end this extends;
`specs/design-program.md` for the safety analysis it generalizes;
`specs/design-plan.md` and `specs/design-planner.md` for the operation it
adds and where the planner puts it; `specs/design-eval.md` for the executor
and the batch; `specs/design-parallel.md` for the frozen round a computed
value must not write into; `specs/design-memory.md` section 8 and
`specs/design-comparison-cost.md` section 7 for the integer table;
`specs/design-magic.md` for the rewrite; `specs/decisions.md` D-3, D-4,
D-7, D-11, D-16 and D-24.

---

## 1. What this document settles

* what an expression is, where one may be written, and how `-` stops being
  part of an integer literal without changing what any existing program
  means;
* what an expression is worth: 64-bit integers, which operations are
  defined on them, what a comparison over an expression with no value does,
  and why an operation whose result cannot be represented fails the
  evaluation rather than dropping a binding;
* when `=` binds a variable rather than testing it, and how the safety
  analysis of `specs/design-program.md` grows to admit it while staying one
  set computation;
* why the answer of a program that succeeds does not depend on which
  equality binds, on the strategy, or on the thread count, and exactly what
  may depend on them;
* how a value the executor computes, rather than reads from a tuple, lives
  in the binding frame, reaches a head tuple and becomes an interned
  identifier -- without the executor writing to the context, so that the
  frozen round of `specs/design-parallel.md` keeps its contract and a
  declined merge leaves the value table as it found it;
* what interning during evaluation costs, and which limit bounds it;
* the plan operation this adds, where the planner places it, and what the
  Magic rewrite does with it;
* the diagnostics, the limits, the explain text and the tests.

It does not add aggregates, functions over strings, or any value type. Those
are separate features with designs of their own, and section 19 says why
none of them is smuggled in here.

---

## 2. Why arithmetic, and why this much

Architecture section 69 puts arithmetic first among the post-v1 features
because it decides which programs can be written at all. Without it a rule
cannot count, measure or step:

```prolog
% Distance along a dependency chain, bounded so that it terminates.
dist(X, Y, 1) :-
    edge(X, Y).
dist(X, Z, D1) :-
    dist(X, Y, D),
    edge(Y, Z),
    D < 8,
    D1 = D + 1.

% Fields laid out in a struct: the end of each field from its offset.
field_end(F, E) :-
    field_offset(F, O),
    field_size(F, S),
    E = O + S.

% Tasks whose estimate exceeds twice what their owner has left.
overcommitted(T) :-
    estimate(T, E),
    owner(T, U),
    capacity(U, C),
    E > C * 2.
```

Every example is integer arithmetic on values a positive literal already
bound, compared or assigned to one variable. That is the feature: **an
integer expression may stand where a term stands in a comparison, and an
equality whose one side is a variable nothing else binds gives that variable
the other side's value.** Nothing else about the language changes. Heads and
atom arguments still hold terms, values are still symbols, strings and
integers, and a rule is still a conjunction of literals over relations.

Architecture section 71 asks for no feature without a clear Datalog use, and
that is why the expression language is small: `+`, `-`, `*`, `/`, `mod`,
unary `-` and parentheses, over integers and variables. Each of the things
left out, from bitwise operators to `min`, has a use somewhere; none has one
that the workloads of `specs/design-bench.md` or the programs of
architecture section 4 need, and each is a later addition that this design
leaves room for rather than a decision it has to make now.

---

## 3. Syntax

### 3.1 Tokens

One token joins the lexer's list of `specs/design-parser.md` section 3.2,
and one changes:

```text
operator        + - * /, one token kind carrying which
integer         [0-9]+, converted to int64_t -- no longer -?[0-9]+
```

Operators are one token kind with an operator field, as comparisons are: the
parser wants to know which operator it holds only when it stores it.

**`-` is always its own token.** D-16 folded a `-` immediately before a digit
into the literal because "v1 has no arithmetic, so a `-` cannot be an
operator". With arithmetic it can: `X-1` must be a subtraction, and a lexer
that had already made `-1` a literal would report an unexpected integer where
the writer sees an expression. The lexer therefore never looks past a `-`,
and a negative integer is a matter for the parser (section 3.4).

**The remainder operator is spelled `mod`, not `%`.** `%` begins a comment
wherever a token may begin, and it keeps doing so: a lexer that made `%` an
operator inside an expression and a comment outside one would need to be
told by the parser which it is in, a mode bit threaded between the two
modules that `specs/design-parser.md` section 2 keeps apart, and every
program with a comment after an expression would be one missing space away
from a different program. `mod` is an ordinary symbol token that the parser
recognizes as the operator **only where an operator may stand**, after an
operand, exactly as it recognizes `not` only where a literal may begin. A
symbol can never legally follow an operand without a comma between, so the
position decides and nothing is reserved: `status(foo, mod).` stores the
symbol `mod`, and a predicate may be called `mod`, since at the start of a
literal the word is a name as it always was. `X mod 2` is a remainder.

### 3.2 Expressions

```text
comparison   := expression comparator expression

expression   := product (("+" | "-") product)*
product      := unary (("*" | "/" | "mod") unary)*
unary        := "-" unary
              | primary
primary      := integer
              | variable
              | symbol
              | string
              | "(" expression ")"
```

Binary operators are left-associative, `*`, `/` and `mod` bind tighter than
`+` and `-`, and unary `-` binds tighter than any of them, so `-X * 2` is
`(-X) * 2` and `A - B - C` is `(A - B) - C`. These are the conventions of C
and of every Datalog with arithmetic, and a program that means something
else writes parentheses. `mod` is recognized as an operator only in the
position the grammar shows it, after a `unary`; a `mod` where an operand is
expected is the symbol constant.

An expression whose every operand is a constant is still an expression; the
evaluator computes it for every binding that reaches it. Constant folding is
an optimization with no effect on any answer and is left for a later stage
that measures a need for it.

### 3.3 Where an expression may stand

**Only as an operand of a comparison.** A rule head, a fact and an atom
argument hold terms, as architecture section 13 has them: a variable, a
symbol, a string or an integer. So

```prolog
succ(X + 1) :- n(X).          % rejected: expected a term
p(X) :- q(X, Y + 1).          % rejected: expected ',' or ')', found '+'
succ(Y) :- n(X), Y = X + 1.   % accepted
```

The restriction is not caution; it is what keeps one construct meaning one
thing. An expression in a head would be sugar for a fresh variable and an
assignment in the body, and sugar has to be expanded somewhere: the compiled
rule would hold a variable the writer never wrote, `-e` would show a rule
that is not the one in the file, the singleton warning would have to know
which variables are synthetic, and the query's output columns of
`specs/design-plan.md` section 7 -- the named variables in first-occurrence
order -- would need a rule for the unnamed one. The assignment form says what
the program means, in the vocabulary the rest of the language uses, and it
is what the plan runs anyway.

A comparison may compare two expressions, an expression and a term, or two
terms as before. `N >= 100`, `a != b` and `"left" < "right"` parse as they
do today, since a term is an expression of one operand.

### 3.4 Negative literals

A `-` followed by an integer token is a **negative integer literal** wherever
a term may stand: in a fact, a head, an atom argument or a comparison
operand. `offset(start, -1).` means what it means today, and so does
`offset(start, - 1).`, which was an error and is now the same fact, since
the parser rather than the lexer joins the sign to the digits.

Inside an expression the same two tokens are folded the same way, so
`X = -1` assigns the constant -1 rather than negating the constant 1, and
`X - -1` is `X - (-1)`. The fold is what keeps `INT64_MIN` writable: the
lexer converts a magnitude, which is defined for `9223372036854775808` only
when a `-` precedes it, and the parser applies the sign and checks the
range with the sign known. The integer rule of `specs/design-import.md`
section 4, `-?[0-9]+` within `int64_t`, is unchanged for the importer and is
now composed of two pieces the lexer and the parser each use: a magnitude
conversion over a digit range, and the signed range check. The lexer's
"identifier byte after the digits" check stands, so `0x10` is still one
error rather than two tokens.

Every program v1 accepts parses to the same tree. The only place a `-`
could legally appear in v1 was directly before the digits of a literal, and
the parser now produces that literal from the two tokens. What v1 rejected
and this design accepts -- a `-` with space before the digits, and every
expression -- is new text, so no existing program changes meaning.

### 3.5 What is checked while parsing

The parser owns three checks an expression can fail, each a diagnostic at
the byte the reader should look at and each subject to the one-message-per-
statement rule of `specs/design-parser.md` section 7:

* **an operand of an operator is a symbol or a string.** A constant's type
  is a property of its token, so `foo + 1` and `"a" * 2` are mistakes the
  parser can see, and a mistake the parser can see is reported where it is
  written rather than left to fail silently for every binding at run time.
  A variable operand is not checked here: a variable's type is a property
  of the tuples that bind it, and section 4.2 settles what happens then.
* **an expression is nested more than 32 levels deep.** One constant,
  `RELIC_MAX_EXPRESSION_DEPTH`, bounds two things that are not the same and
  are each a stack:
  - **the descent of the text**: the parser counts the `(` and unary `-`
    productions it is inside and refuses the 33rd where it opens, before
    entering it. This is what bounds the parser's own recursion, and it has
    to be counted in the text rather than in the tree, because `((((X))))`
    builds a tree of one node however many parentheses it has, and `- - -X`
    descends three times before a node exists;
  - **the height of the tree**: every operator and negation counts one
    level, and the parser refuses the node that would be the 33rd where its
    operator is written. A chain `A + B + ...` of 33 operands is a tree 33
    deep, since it associates to the left, and `X + X * (X + X * (...))` is
    twice as deep as its parentheses. The height is what bounds every walk
    over the tree -- the printer's, the rewrite's, the planner's -- and the
    evaluator's stack (section 7.4).
  Both are refused with one message, since a reader fixes both the same
  way, by splitting the expression across assignments. Section 13 says why
  the bound is structural.
* **an operand is missing.** `X + ,`, `(X + 1`, `X = ` are the ordinary
  "expected an operand, found ','" family, worded as the parser words every
  other expectation: what was expected and what was found.

An expression that parses is a **tree**, the one structure
`specs/design-parser.md` section 11 said the language did not have: a node is
a constant, a variable, a negation of one node or an operator over two, and
a comparison holds two of them where it held two terms. The tree is a
transcription of the text, like the rest of the syntax tree, and it is what
`-e` prints a rule from, so it keeps the shape the writer gave it.

The number of nodes is not bounded, only the height: a tree of height 32
may in principle hold billions of nodes, and the text that writes them is
what bounds it, as it bounds a body's length. Every count of steps or nodes
is a `size_t`, and section 10 says what a large expression costs the Magic
rewrite's budgets.

---

## 4. Meaning

### 4.1 Integers

An expression's value is a signed 64-bit integer, the language's one
numeric type. The operators are:

| Operator | Value | Defined when |
| --- | --- | --- |
| `A + B`, `A - B`, `A * B` | the exact sum, difference or product | the result fits `int64_t` |
| `A / B` | the quotient, truncated toward zero | `B != 0`, and the result fits |
| `A mod B` | `A - (A / B) * B`, so its sign is `A`'s | exactly when `A / B` is |
| `-A` | the negation | the result fits |

Truncating division and a remainder with the dividend's sign are C's rules,
and Souffle's, and SQL's; a program that wants floor division writes it. The
one place C's rule is not followed is the undefined case: `INT64_MIN / -1`
and `INT64_MIN mod -1` overflow, in C as undefined behaviour and on x86 as a
trap, and here as the error of section 4.2. The remainder is tied to the
quotient rather than given a value of its own because a rule that reads
"defined exactly when the quotient is" has one case fewer than one that says
"except when the divisor is -1, when it is zero".

### 4.2 An operand that is not an integer, and a result that is not one

Two different things can go wrong when an expression is evaluated, and
they are treated differently on purpose.

**An operand is a symbol or a string.** Predicate arguments are untyped, and
architecture section 10.1 decided that a column holding a symbol next to an
integer is a fact about the data rather than a mistake: an ordering
comparison between the two is false and the binding is filtered out.
Arithmetic follows the same rule. **An expression one of whose operands is
not an integer has no value, and a comparison or assignment over it is
false**, so the binding is dropped. The note of verbosity level 2,
`RELIC_F_NOTE_MIXED`, covers it as it covers the ordering case, once per
rule:

```text
sizes.dl:4:22: note: operand of + is a symbol; the comparison is false
```

This is a defined answer over the language's three types, not a missing
one: a symbol plus one is not greater than a hundred, as a symbol is not
greater than a hundred today.

**A result does not fit.** Overflow and a zero divisor are different in
kind: the program meant an integer and the machine cannot hold it. **An
operation whose result is not defined by section 4.1 is an evaluation
error.** The evaluation stops where a limit would stop it -- no rows, a
nonzero exit, the relations stale so that the next evaluation starts from
the facts -- and the diagnostic names the operation and the rule:

```text
depth.dl:7:14: error: integer overflow in rule dist/3
budget.dl:12:9: error: division by zero in rule average/2
```

**Why an error and not a dropped binding.** Dropping the binding, as the
type case does, looks conservative -- the answer says less, never something
false -- and it is not, because the language has negation. With
`score(a, 9223372036854775807)`,

```prolog
blocked(X) :- score(X, S), S + 1 > 100.
allowed(X) :- score(X, _), not blocked(X).
```

a dropped binding leaves `blocked(a)` underived, and `allowed(a)` is then
derived: an affirmative answer that the program, read over the integers,
denies. A tool whose answers may decide what a user is allowed to do cannot
say `allowed(a)` because a number was too large. The type case has no such
problem, because a symbol operand is not a lost integer: `blocked` over a
symbol score is genuinely false in the three-typed language, and
`allowed` is the answer the program means. An undefined result has no
answer the program means, so the only honest output is none.

**Why an error and not wrap-around.** C's unsigned rule and Souffle's,
wrapping turns a count that grew too large into a small number and derives
tuples that are false, silently; saturation does the same with a different
wrong number. Both are worse than the dropped binding above, since they
produce false answers with and without negation.

**What the error costs, stated plainly.** Whether an evaluation meets an
undefined result depends on which bindings its expressions are evaluated
over, and that depends on the plan and the strategy. Full materialization of
`p(X, Y) :- n(X), Y = 1 / X` over `n(0). n(1).` meets the zero; the Magic
rewrite of `p(1, Y)?` never does. A filter placed before an expression drops
bindings the expression would have overflowed on, and `-X no-reorder`
places differently. So **a strategy that evaluates more bindings may fail
where one that evaluates fewer succeeds.** Three contracts are amended to
say so, and section 4.5 states what does not change: every strategy that
succeeds gives the same answer, and it is the answer over the integers.

* The equivalences of architecture section 66.2 hold on the answers of the
  strategies that succeed. A program on which some strategy meets an
  undefined result is compared by golden test, not by the differential
  harness, and the generator of `specs/design-hardening.md` produces no such
  program (section 15).
* `specs/design-magic.md` section 8 promises that an abandoned rewrite,
  answered from the whole program instead, changes nothing but the cost. It
  changes one more thing: the whole program may meet an undefined result
  the rewrite would not have reached. The fallback still never gives a
  different answer, only possibly none.
* `specs/design-eval.md` section 7 delivers no row until nothing can fail.
  A plan holding an arithmetic operation can fail on any binding, so **a
  query whose plan holds one is gathered into its answer relation and never
  streamed**, section 9.3, and the promise holds as written.

This is the position every SQL engine takes -- a division by zero in a row
a query never needs is not an error, and one in a row it needs is -- and it
is taken here for the same reason: the alternative is either a false answer
or refusing to optimize.

### 4.3 Comparisons over expressions

`=` and `!=` remain total and `<`, `<=`, `>`, `>=` remain defined within a
type, per D-11, extended to operands that are computed:

* two terms compare as they do today -- equality by identifier, ordering by
  decoded value, cross-type ordering false;
* a computed integer against a computed integer compares numerically;
* a computed integer against a term is equal to it exactly when the term is
  an integer of that value, and ordered against it exactly when the term is
  an integer; a term that is a symbol or a string is unequal and unordered.

So `X + 0 = X` holds for every integer `X` and for no symbol, and
`X + 1 > foo` is false and noted. Equality by identifier is exact because
values are interned; equality between a computed integer and an identifier
decodes the identifier, since the computed side has none yet (section 8.2).

### 4.4 An equality that binds

Architecture section 10 made `=` a filter because comparisons did not
generate values; D-11 records it. An expression generates one, and the
equality that names it is how the value gets a name:

> **An equality one side of which is a variable that nothing before it has
> bound, and whose other side is fully bound, binds that variable to the
> other side's value.** Such a literal is an **assignment**. Any other
> equality is a filter, as before.

"Before it" is the plan's order, section 7.1, and "bound" is what the
literals placed before it bound, positive literals and assignments alike.
The variable may take a computed integer, `D1 = D + 1`; a constant, `X = 42`
or `X = foo`; or another variable's value, `Y = X`. The other side may be
any expression or term whose variables are all bound. An assignment whose
expression has no value binds nothing and drops the binding, section 4.2.

Three things follow.

**`X = 42` is no longer unsafe.** `answer(X) :- X = 42.` derives `answer(42)`,
where v1 rejected it. That is an extension: a program v1 accepted contains
no assignment, since every variable of every comparison in it is bound by a
positive literal, so its equalities stay filters and it derives what it
derived. Section 7.1 has one qualification, for the plan and not the answer.

**Both sides free is an error.** `X = Y` where neither is bound elsewhere
binds nothing, and both variables are reported as unsafe (section 5).
`X = Y + 1` with `Y` free is the same: the expression is not fully bound,
so the literal is not an assignment, and `Y` is unsafe.

**Only a bare variable is assigned.** `X + 1 = Y` binds `Y` when `X` is bound
and `Y` free, and binds nothing when `Y` is bound and `X` free: relic does
not solve equations, and an equality whose variable side is inside an
expression is a filter. The rule is simple to state, simple to plan and
matches what every Datalog with arithmetic does; inverting an operator is
left absent (section 19) rather than half-done for `+` and `-`.

**An aggregate literal is a second form of assignment.** `N = count { ...
}` binds `N` once every variable it shares with the rule is bound, and is
a filter when `N` is bound before it, by the same rule of the plan's order;
`specs/design-aggregates.md` section 4.6 states it, and section 4.5's
argument below covers it unchanged.

### 4.5 Why the order of binding does not matter

A body's comparisons are constraints, and the plan chooses which of the
equalities it evaluates as assignments and in which order. The answer does
not depend on the choice:

> Fix a body and a tuple for each positive literal. Every schedule the plan
> may choose either meets an undefined result, or produces, for that tuple,
> exactly the bindings of the rule's variables that satisfy every comparison
> of the body with every expression having a value.
>
> *Every binding a schedule produces satisfies every comparison*: a
> comparison evaluated as a filter held, and one evaluated as an assignment
> holds by construction, because it defined its variable to make it hold;
> an expression with no value made its literal false and the binding was
> dropped, so no produced binding has one. *Every satisfying binding is
> produced*: an assignment's variable has, in a satisfying binding, exactly
> the value the other side computes, since expressions are functions; so
> the schedule computes that value, and each filter, evaluated over a
> satisfying binding, holds.

So **every schedule that succeeds derives the same tuples, and they are the
tuples the program derives over the integers**: a successful run evaluated
every expression it needed, and each had the exact value. Two schedules
differ only in whether one meets an undefined result the other does not
reach, section 4.2, and in which rule's type note is issued first. This is
what lets the planner place an assignment wherever it is cheapest, and lets
`-X no-reorder`, the Magic rewrite, the naive evaluator and a parallel round
each place it their own way while every strategy that answers gives the
same answer, byte for byte.

### 4.6 Termination

A rule that computes a new integer from an old one can derive a relation
with no fixed point in any practical sense:

```prolog
n(0).
n(Y) :- n(X), Y = X + 1.
```

Architecture section 15's least fixed point exists over the integers and
not over `int64_t`: the rule overflows at `INT64_MAX`, 2^63 tuples away.
`specs/design-eval.md` section 3's termination argument, that every value an
insertion can produce was interned before evaluation began, no longer
holds: an assignment interns what it computes. **Termination becomes the
program's responsibility**, as it is in every Datalog with arithmetic, and
the tool's part is the tuple and memory limits of architecture section 63,
which stop such a program with a diagnostic rather than letting it run to
exhaustion. A recursive rule that computes should carry a bound, as `dist/3`
in section 2 does, and the manual page says so where it introduces
arithmetic.

---

## 5. Safety

`specs/design-program.md` section 5 states range restriction as one set
computation: with *B* the slots bound by positive literals and *R* the slots
of the head, of negated literals and of comparisons, the rule is safe when
`R \ B` is empty. The argument that no ordering search is needed was that a
positive literal needs nothing bound in order to be scanned, so placing every
positive literal first is always a legal order.

An assignment does need something bound, so *B* grows by a closure rather
than one union:

```text
B = slots of every positive literal
repeat until B does not change:
	for each equality, in written order, with one side a bare
	    variable v not in B and every slot of the other side in B:
		add v to B
R = slots of the head, of every negated literal, of every comparison
safe exactly when R \ B = {}
```

Each pass either adds a slot or ends, so there are at most 65 passes over
the comparisons, 64 that add a slot and one that finds nothing to add; a
rule with no assignment ends after one, at the cost the
check has today. The closure is monotone, so its result is the same in
whatever order the equalities are tried, and the order written is used
because it is the one a reader can reconstruct.

**The closure is what makes every plan placeable.** The planner walks the
body keeping the set of slots bound so far, and at every point places every
test whose slots are bound and every assignment whose operand slots are
bound. Whatever order it chose for the positive literals, once all of them
are placed the bound set contains the closure's *B*: every assignment the
closure fired can fire, in the closure's order if no other, and then every
filter can be placed. So the planner never has to ask whether a test can be
placed; safety already said so, which is the property the original
computation had and the reason to keep the check a closure rather than an
ordering search.

The offenders are `R & ~B`, reported in slot order at each variable's first
occurrence, and section 5.1 of the program design still applies: a variable
in `R \ B` occurs in no positive literal and is assigned by no equality, so
every occurrence is required and its first occurrence is what the message
points at. The wording gains the second way a variable can be bound:

```text
graph.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal or an assignment
graph.dl:3:5: error: anonymous variable in rule foo/1 cannot be bound by a positive body literal
```

An anonymous variable keeps its own message. `_ = X + 1` would compute and
discard a value, which no program needs, and `_` is never an assignment's
target: each `_` is a distinct variable in a required position, so the
closure never adds it and the check reports it as it does today.

A variable an assignment binds and nothing reads is a singleton, mentioned
once, and draws the singleton warning of the program design's section 6. It
is the arithmetic form of the typo that warning exists for: a value
computed and never used.

The query rule is checked by the same closure, since its head is its output
columns; `v(X), Y = X * 2?` has output columns `X` and `Y` and is safe.

The closure has a second clause, for the aggregate literal of
`specs/design-aggregates.md` section 5.3: an aggregate whose result
variable is not in *B* and whose every shared slot is adds its result slot
to *B*, and its shared and result slots join *R*. It is monotone like the
first, so the bound on passes and the placement argument above stand.

---

## 6. The compiled rule

The compiled comparison of `specs/design-program.md` section 8 held two
arguments and an operator. It holds two **expressions**:

```c
enum relic_expr_kind {
	RELIC_EXPR_CONSTANT,		/* an interned value */
	RELIC_EXPR_VARIABLE,		/* a slot */
	RELIC_EXPR_NEGATE,		/* -operand */
	RELIC_EXPR_BINARY		/* left op right */
};

struct relic_expr {
	union {
		relic_dom_t		 constant;
		uint16_t		 slot;
		relic_expr_t		*operand;	/* NEGATE */
		struct {
			relic_expr_t	*left;
			relic_expr_t	*right;
			enum relic_operator	op;
		}			 binary;
	}			u;
	enum relic_expr_kind	kind;
	uint8_t			height;		/* leaves are 1 */
	size_t			nodes;		/* this node and below */
};

struct relic_comparison {
	relic_expr_t		*left;
	relic_expr_t		*right;
	enum relic_compare_op	 op;
};
```

Nodes live in the program arena beside the rule, as its arguments do, so an
expression lives exactly as long as its rule. A term is a one-node tree, so a
comparison of two terms costs two nodes where it cost two arguments, and the
compiled form of every v1 program has the same shape it has now with one
more level of indirection on comparisons. A node records its height and its
node count so that a walk can size what it needs, and a budget charge what
it costs, without a walk of their own.

The tree is kept, rather than compiled here to the flat form the executor
runs (section 8.1), because three consumers want the shape: `-e` prints a
rule from its compiled form and must print `(D + 1) * 2` with the
parentheses the tree requires; the Magic rewrite renumbers a rule's slots
by walking its literals and compares generated rules structurally to drop
duplicates, `specs/design-magic.md` section 7; and the planner reads the
slots an expression mentions. Each is a walk over a tree of height at most
32. The flat form is the plan's, built when the plan is, in the evaluation
arena, per section 7.4.

An integer constant under an operator, the `1` of `X + 1`, is interned as
every integer literal is, per `specs/design-parser.md` section 6: the
parser interns values as it builds terms, and an expression's constant is a
term. A program that never compares a computed value against `1` has
interned an integer it does not need, which costs a table entry and keeps
one rule for constants instead of two.

---

## 7. Planning

### 7.1 Placement

`specs/design-plan.md` section 5 turns each literal into an operation at the
point it is placed, walking a bound set. One rule is added:

* an **equality** whose one side is a bare variable not in the bound set and
  whose other side's slots are all in it becomes `ASSIGN`, and adds its
  variable to the bound set; every other comparison becomes `FILTER`, as
  before, and is placed where every slot it reads is bound.

Tests are placed at the earliest point they can be, in written order among
those placeable at the same point, exactly as today; what is new is that
placing an assignment can make further tests placeable, so the placement
repeats until nothing more is, before the next positive literal is scored.
`D1 = D + 1, D2 = D1 * 2` places both after whatever binds `D`, in that
order, whether they were written in it or not.

The greedy order of `specs/design-planner.md` section 5.1 is untouched:
positive literals are still scored by bound arguments, estimate and
position, against the bound set as it stands, which now may hold slots an
assignment bound. A literal keyed on such a slot has a bound argument and
is scored and accessed accordingly (section 7.3).

`specs/design-planning-cost.md` section 3 rests on the bound set growing
monotonically through at most 65 states, and an assignment adds a bit as a
positive literal does, so the one-pass-per-state ranking there holds with
no change: a test becoming ready is still a function of the mask, and an
assignment placed is a mask change like any other.

**The one qualification to "v1 programs derive what they derived"** is the
plan. In `p(X), q(Y), X = Y`, the closure of section 5 binds both variables
by positive literals, but at the point after `p(X)` is placed, `Y` is not
yet bound, `X` is, and the rule above makes `X = Y` an `ASSIGN` of `Y` and
`q(Y)` a `MEMBER`. The rows are the same by section 4.5; the plan is better,
one probe per `X` instead of a scan of `q`, and `-e` shows the new plan. The
golden explain tests of rules with an equality between the variables of two
literals change with it, and are re-recorded.

### 7.2 The two kinds of slot

A slot bound by a generator holds a domain identifier read from a tuple. A
slot bound by an assignment from an expression holds an integer the executor
computed, which has no identifier yet and may never need one. The frame
therefore has two arrays, and every slot has a **kind** the plan fixes:

* a **domain slot** is bound by a generator, or by an assignment from a
  constant or from another domain slot; its frame entry is an identifier;
* a **number slot** is bound by an assignment from an expression, or from
  another number slot; its `numbers` entry is the integer, and its frame
  entry is the integer's identifier when the value table holds one and zero
  otherwise (section 8.2).

An aggregate literal binds a slot of one kind or the other by its
function, `specs/design-aggregates.md` section 8.3: `count` and `sum` bind
a number slot, their result being an integer the executor computed, and
`min` and `max` bind a domain slot, their result being a value the group
holds.

The kind is static because the binding is: a slot is bound by exactly one
operation of a plan, the first that reaches it, and every later occurrence
reads. The planner knows which operation that is and what kind it produces,
and writes the kind into every read of the slot, so the executor never asks.
This is the term-action rule of `specs/design-plan.md` section 4 applied to
one more fact.

The kind is a fact about a plan, not about the rule. In `p(X), Y = X + 1,
q(Y)` the closure binds `Y` through `q` and the planner assigns it after
`p`, so `Y` is a number slot in this plan and would be a domain slot in a
plan that placed `q` first. That is why the flat form of an expression is
built with the plan (section 7.4): its reads name the slot's kind, and the
kind is decided when the order is.

### 7.3 Access paths

A positive literal whose bound columns include a number slot is looked up,
tested for membership or scanned by the rule of `specs/design-planner.md`
section 5.2, through the slot's frame entry. `dist(X, Z, D1) :- dist(X, Y,
D), edge(Y, Z), D < 8, D1 = D + 1` is planned as

```text
scan dist(X, Y, D)
filter D < 8
assign D1 = D + 1
lookup edge(Y, Z) key={0}
emit dist(X, Z, D1)
```

and `succ_edge(X, Z) :- n(X), Y = X + 1, edge(Y, Z)` looks `edge` up on `Y`,
a number slot, with the key built from its frame entry.

### 7.4 The operations

```c
enum relic_op_kind {
	RELIC_OP_SCAN,
	RELIC_OP_LOOKUP,
	RELIC_OP_MEMBER,
	RELIC_OP_ANTI_MEMBER,
	RELIC_OP_FILTER,	/* does a comparison of two values hold? */
	RELIC_OP_ASSIGN,	/* bind a slot to a value */
	RELIC_OP_EMIT
};
```

| Operation | Reads | Binds | Succeeds |
| --- | --- | --- | --- |
| `FILTER` | both operands' slots and constants | nothing | once or never |
| `ASSIGN` | the expression's slots and constants | its slot | once or never |

`ASSIGN` is a test that binds: it reads bound values, produces at most one
match, and leaves one new binding in the frame. It never runs out of matches
as a generator does; it fails only when its expression has no value
(section 4.2), and then the binding is rejected as a filter rejects one.

An operand is a **program**, the flat form of an expression:

```c
enum relic_step_kind {
	RELIC_STEP_CONSTANT,		/* push an interned value */
	RELIC_STEP_DOMAIN_SLOT,		/* push a domain slot's identifier */
	RELIC_STEP_NUMBER_SLOT,		/* push a number slot's integer */
	RELIC_STEP_NEGATE,		/* pop one integer, push its negation */
	RELIC_STEP_ADD, RELIC_STEP_SUBTRACT, RELIC_STEP_MULTIPLY,
	RELIC_STEP_DIVIDE, RELIC_STEP_REMAINDER
					/* pop two integers, push the result */
};

struct relic_step {
	union {
		relic_dom_t	constant;
		uint16_t	slot;
	}			u;
	enum relic_step_kind	kind;
};

struct relic_program {
	relic_step_t	*steps;		/* postfix order */
	size_t		 step_count;
	uint8_t		 depth;		/* stack cells the run needs */
};

struct relic_op_filter {
	relic_program_t		 left, right;
	relic_location_t	 where;
	enum relic_compare_op	 compare;
};

struct relic_op_assign {
	relic_program_t		 value;
	relic_location_t	 where;
	uint16_t		 slot;
};
```

A program is the expression's tree in postfix order, built by one walk when
the operation is emitted, into the evaluation arena with the rest of the
plan. A term is a one-step program. Evaluating a postfix program of a tree
of height *h* needs at most *h* + 1 stack cells -- a node's left subtree
leaves one cell and its right subtree then needs at most its own height
plus one -- so the walk records the tree's height plus one as `depth`, at
most `RELIC_MAX_EXPRESSION_DEPTH + 1`, and the executor's stack is an array
of that many cells. The flat form is chosen over walking the tree at run
time for the reason `specs/design-plan.md` section 2 gives for plans: an
array that states every decision in advance, run by a loop with no recursion
and no branch on what kind of slot this is.

An `EMIT`'s head columns gain one action beside the four of
`specs/design-plan.md` section 4:

```c
enum relic_term_action {
	RELIC_PLAN_CONSTANT,
	RELIC_PLAN_BOUND,	/* a domain slot's identifier */
	RELIC_PLAN_NUMBER,	/* a number slot: its identifier, or pending */
	RELIC_PLAN_BIND,
	RELIC_PLAN_ANY
};
```

`NUMBER` occurs only in an `EMIT`. Every other operation reads a number
slot through `BOUND`, section 8.2, and a generator never binds one.

The three invariants of `specs/design-plan.md` section 6 hold, with the
second read as "some earlier operation binds, a generator or an `ASSIGN`".

---

## 8. Execution

### 8.1 Evaluating an expression

A program runs on a stack of cells in the executor scratch:

```c
struct relic_cell {
	union {
		relic_dom_t	dom;
		int64_t		number;
	}		u;
	bool		is_number;
};
```

`CONSTANT` and `DOMAIN_SLOT` push an identifier; `NUMBER_SLOT` pushes an
integer. An arithmetic step pops its operands as integers: a cell that holds
an integer is that integer, and a cell that holds an identifier is decoded
through the integer table -- a read of `integers[k - 1]` for an identifier
whose type bits say integer, and **no value** for a symbol or a string. A
step with no value ends the run with no value; the operation that ran it
rejects the binding and records the note of section 4.2. A step whose
result is not defined by section 4.1 ends the run with an **error**,
section 8.4.

Overflow is detected before it happens, never observed after: the checked
forms `__builtin_add_overflow()` and its siblings where the compiler has
them, which `configure` tests for, and the portable comparisons against the
bounds otherwise, in `compat/`, under the same unit tests. Division checks
its divisor for zero and the pair for `INT64_MIN / -1` before dividing, so
no instruction the hardware traps on is ever issued. There is no signed
overflow in the evaluator for UBSan to find, and `make check-ubsan` is run
over the arithmetic tests to say so.

A comparison of two cells is the rule of section 4.3: two identifiers as
today; two integers numerically; an integer and an identifier by decoding
the identifier, equal or ordered only if it decodes to an integer. A
`FILTER` runs both programs and compares the two cells left; an `ASSIGN`
runs one and binds its slot from the cell, an identifier into a domain slot
or an integer into a number slot by the slot's kind, which the planner fixed
from the program's result kind.

### 8.2 A computed integer in the frame

A number slot's integer goes into `numbers[slot]`. Its frame entry is set at
the same moment by **one read-only probe of the integer table**:
`relic_value_find_integer()`, the lookup half of interning, which returns
the identifier the table holds for the integer or zero when it holds none.
The function is `const` over the context and joins `lib/frozen.h`, so a task
of a parallel round may call it.

Zero in the frame is what makes every later read of the slot need no case
of its own:

* a **generator or membership test** reading the slot through `BOUND`
  compares a tuple column, or builds a key, with the frame entry. No column
  and no key of any relation holds zero, since zero names no value, so a
  slot whose integer the table lacks matches no tuple -- which is correct,
  because an integer the table lacks is in no relation. `SCAN` and `LOOKUP`
  find nothing, `MEMBER` fails and `ANTI_MEMBER` succeeds, each by its
  ordinary comparison. The index and the canonical index treat a probe's
  columns as opaque words, so a zero among them is a key no chain matches,
  and a unit test probes with one to say so;
* an **expression** reading the slot pushes `numbers[slot]`, never the frame
  entry, so a value that was never interned is still computed with;
* an **`EMIT`** reading the slot through `NUMBER` writes the frame entry when
  it is not zero and otherwise a pending integer, section 8.3.

The probe is not free -- a hash and a tagged probe per assignment -- and it
is what lets the executor stay a reader: an assignment that interned its
result would write the context, and section 9.2 says why it must not. On
`dist/3` it is one probe per derivation beside the insertion's own, which
`specs/design-evaluation-cost.md` section 5 measured at a few tens of
nanoseconds.

### 8.3 A computed integer in a head tuple

A head column read through `NUMBER` whose frame entry is zero names an
integer that is not in the table, so it has no identifier to write into the
batch. The `EMIT` appends the integer to the batch's **pending** array and
writes a **pending marker** in the column: the position, counting from one,
with type bits of zero. Zero type bits are never assigned to a value,
`specs/design-memory.md` section 8, so a marker cannot be mistaken for an
identifier by anything that decodes one; and a marker never reaches a
relation, a hash the store uses or a callback, because every batch is
**resolved** before it goes anywhere. A plan that streams never holds a
number slot, section 9.3, so no path skips resolution.

**An integer's identifier is known before it is assigned.** The table
assigns identifiers in order, `RELIC_DOM_INTEGER | k` for the *k*th distinct
integer it holds, so the identifier a new integer will receive is a function
of the table's count and of the order in which the new integers are met.
Resolution uses that to separate what can fail and what changes the context
from the rewriting of the batch, in three steps that mirror an insertion's
(`specs/design-relations.md` section 6):

```c
size_t	relic_pending_measure(const relic_t *, const int64_t *pending,
	    size_t count, relic_pending_map_t *map);
void	relic_pending_rewrite(const relic_pending_map_t *map,
	    relic_dom_t *columns, size_t tuples, uint16_t arity);
int	relic_pending_commit(relic_t *, const relic_pending_map_t *map);
```

1. The **measure** walks the pending integers in order and fills a **map**
   the caller provides, sized for the batch: for each pending integer, the
   identifier it has, when the table holds it, or the identifier it will
   have, `count + j` for the *j*th distinct new integer met. It returns the
   bytes the table would grow by to hold the new ones, projected from the
   table's growth policy by the function the growth itself uses. It reads
   the table and writes only the map.
2. The **rewrite** writes each marker's identifier from the map over the
   marker. It reads the map and writes only the batch, so a batch whose
   columns are now all identifiers can be hashed, screened and classified
   as any batch is, while the table still holds what it held.
3. The **commit** reserves room in the table for the map's new integers at
   once, growing the `int64_t` array and the identifier table at most once
   -- the one step that can fail, for memory or because the table is full
   -- and then appends the new integers in the map's order, which cannot
   fail and gives each exactly the identifier the map promised. A commit
   that fails leaves the table's contents as they were, with whatever
   capacity the reservation obtained, accounted, as a failed insertion
   leaves a relation.

**The capacity reserved is the one interning the integers one at a time
would reach.** A growable array grows to double or to the count asked for,
whichever is larger, `specs/design-memory.md` section 5, so one reservation
for 128 integers and two for 64 do not reach the same capacity, and a
table that grew by rounds would hold, and charge, different bytes at
sixteen threads than at one. The measure therefore projects, and the commit
reserves, the **stepwise capacity**: the capacity the array and the
identifier table would have after the new integers were appended singly,
computed by the same pure function `relic_relation_bytes_after_inserts()`
uses for a relation (`specs/design-relations.md` section 9). The table then
holds the same capacity whether its integers arrived one per batch, one per
round or all at once, and the bytes the memory limit sees are a function of
the integers held and nothing else.

The serial evaluator, taking a batch, measures, asks the memory limit with
the bytes the measure returned and refuses as an insertion refuses, naming
the predicate being derived into; then rewrites, commits and inserts the
batch as today. So nothing is interned from a batch the limit refuses, and
the relations hold what they held. A batch the tuple limit refuses at its
insertion has had its integers committed, since the commit precedes the
insertion; that is the serial evaluator's behaviour, which is the reference
the parallel round is held to, and it costs a few entries in a table the
next evaluation would fill again.

The parallel merge runs the three steps at three different points, so that
the commit falls in the phase that cannot fail, section 9.2.

The executor "does not insert" per `specs/design-eval.md` section 2.1, and
it does not intern either, for the same reason: both are the taker's, on the
calling thread, after the executor has handed the batch back. The pending
array and the map are sized when the evaluation begins, as the batch is, for
the widest head times the batch length, so no `EMIT` allocates, and a head
with no `NUMBER` column never touches them.

### 8.4 Notes and errors cross the executor's boundary

`specs/design-eval.md` section 2.1 has the executor record a mixed-type note
in its scratch and its caller report it when it takes the batch, because
reporting calls the caller's callback. Two things now cross the boundary,
each with a record of its own:

* the **note** of section 4.2, for an operand that is not an integer, as
  today, with the mark that keeps it to once per rule;
* the **error** of section 4.2, for a result that does not fit. The executor
  records the literal's location, which operation and the rule, ends the
  plan as though every generator were exhausted, and returns. The caller
  finds the record when it takes the batch, reports it through the
  diagnostic callback, discards the batch and fails the evaluation.

An aggregate uses the same two records, `specs/design-aggregates.md`
section 9.3: its note, for a `sum` over a value that is not an integer or
a `min` or `max` over values of two types, is the note record, and a `sum`
whose total does not fit is the error record, with the same marks and the
same path to the caller.

So "nothing the executor does can fail" becomes "the executor fails only by
recording that it did", which is one more flag the caller looks at where it
already looks for the note. In a parallel round a task that records an
error **abandons the job**, as a worker whose buffer could not grow does,
and the round is run again serially, `specs/design-parallel.md` section 4.4.
The rerun meets, in serial order, whatever the serial evaluator would have
met first -- this error, an earlier one, or a limit a batch inserted before
it reaches -- and reports it with the serial message. No worker reports
anything, which keeps the rule of section 8 there intact: every refusal a
program can see is made by the serial evaluator's code, on the calling
thread, and no outcome depends on how tasks fell across threads. The rerun
costs a round that was going to fail anyway.

`specs/design-eval.md` section 8 gains a third reason an evaluation fails,
beside a limit and an allocation, and it is reported where it happens like
the other two.

---

## 9. Interning what evaluation computes

### 9.1 One table, charged

Computed integers go into the integer table of `specs/design-memory.md`
section 8, the one path every integer takes. A second table for derived
integers was considered and rejected: the value `5` computed by a rule and
the value `5` written in a later load would have two identifiers unless
every interning consulted both tables in a fixed order and every discard
knew which table a literal came from, and D-3 rejected the tagged 64-bit
cell for having two encodings of one integer. One table, one identifier per
integer, by construction.

What changes is who pays. `specs/design-relations.md` section 9 leaves the
value table out of `max_memory` because it is "sized by what the caller
wrote", and with arithmetic it is not: `n(Y) :- n(X), Y = X + 1` interns an
integer per derivation. **The integer table's storage -- its `int64_t` array
and the slots and tags of its identifier table -- is charged to `max_memory`
whole**, base and computed integers alike, checked before each growth by
the projection of section 8.3 and refused with the store's message:

```text
relic: limit exceeded: memory=536870912 while deriving n/1
```

Charging the whole table rather than the part evaluation added keeps the
account a sum of what exists instead of a record of who allocated it, which
`specs/design-relations.md` section 9 gives as the reason its own total is
reconciled rather than bookkept. A program with a literal in it counts a
few bytes more against its limit than it did; the golden tests of the
memory limit are re-recorded at their new totals.

**The table is not shrunk.** An integer once interned keeps its identifier
for the life of the context, as every identifier does. A discard empties
the derived relations and leaves the table, so a context that has evaluated
a program computing a million distinct integers holds them, about twenty
bytes each, until it is destroyed -- whether or not the next program
computes them again. This is stated rather than hidden, and it is bounded:
the memory limit bounds the table with everything else, and what a
derivation interned a derivation's tuple held, which the same limit
admitted. Reclaiming integers no relation and no statement holds would need
a count per value or a sweep over every base store at every discard, and
section 19 leaves it absent until a workload shows the cost.

### 9.2 The frozen round

`specs/design-parallel.md` section 3 runs a round's plans over a store
nothing writes, and section 7.1 has the compiler enforce that a task cannot
write the context: the executor is compiled against `lib/frozen.h`, where
the context is an incomplete type. An assignment that interned would not
compile, and that is the design working as intended. Sections 8.2 and 8.3
are what an executor that cannot write does with an integer it has just
made: keep it in its frame, look it up read-only, and hand it to whoever
stores the tuple.

The round's two screens, `specs/design-parallel.md` section 5.3, are
adjusted for a candidate with a pending column:

* **against the frozen store, it is new**, and is not looked up. The table
  was frozen with the store, so an integer the table lacked at the
  assignment's probe is in no relation the round reads;
* **against the worker's own candidates**, it is hashed and compared **by
  value**: a pending column contributes a hash of its integer where a
  resolved column contributes its identifier, and two candidates are equal
  when their identifiers agree and their pending integers agree. The screen
  is the worker's own, so its hash need only be consistent within the
  worker, and it is: within one round an integer is either in the table,
  and every candidate holding it holds its identifier, or not, and every
  candidate holding it holds it pending.

The worker's candidate buffer carries its pending integers beside its
tuples, as the batch does. **The merge publishes nothing to the table until
its last phase**, and the three steps of section 8.3 are what make that
possible:

1. **Measure**, before anything is modified: worker 0 walks every task's
   pending integers in task order, filling one map for the round, so that
   each new integer's promised identifier is its position in the serial
   order of first occurrence. The bytes the measure returns join the growth
   the merge projects for the relations, `specs/design-parallel.md` section
   6.1, and the limit is asked about both at once, with the buffers still
   charged. A round that would not fit is declined as before: buffers
   released, table untouched, the round run again serially.
2. **Rewrite**, still before anything is modified: the markers in the
   candidate buffers are replaced by the identifiers the map promised, and a
   candidate that held one is hashed over its identifiers. The candidates
   are now ordinary tuples with hashes, so the merge's classification and
   its tuple-limit check, `specs/design-parallel.md` section 6.3, run over
   them unchanged. Every point at which the merge may still decline --
   the tuple limit, an allocation, a reservation -- comes after this step
   and before the next, and at each of them the table holds what it held
   when the round began, so a declined round leaves it exactly as the
   serial evaluator finds it for the rerun.
3. **Commit**, in the merge's reservation phase: the table's reservation is
   made beside the relations', to the stepwise capacity of section 8.3, and
   can fail only for memory, which declines the round as a failed relation
   reservation does, with the table's contents unchanged; the appends
   follow in the phase that cannot fail, beside the relations' appends, and
   give each integer the identifier the rewrite already wrote into its
   tuples.

So the rule of `specs/design-parallel.md` section 8 stands: a parallel round
either completes its merge or is run again serially, and every refusal a
program can see is made by the serial evaluator's code, on the calling
thread, in the serial order, with the serial message. The memory limit's
admission covers the table's growth as it covers the relations', so an
admitted merge cannot be refused by it during the merge; and nothing the
merge does before its last phase changes the table, so nothing it declines
has to be undone.

**The identifiers are the same at every thread count.** The serial
evaluator interns an integer when it commits the first batch holding it;
the merge interns it when it commits the round, with the identifier the
measure promised, which is its position in the serial order of first
occurrence among the candidates -- and the candidates in task order are the
serial derivation order with duplicates removed: duplicates of tuples the
store held, whose integers were interned already, or of earlier candidates
of the same worker, which come first in the order. So the first occurrence
is the same tuple in both, the table gains the same integers in the same
order, and `relic_stats()` reports the same `integer_values`; and the table
reaches the same capacity, since both reserve the stepwise one, so
`storage_bytes` and every later memory-limit decision are the same too. A
round that is declined interned nothing and is rerun by the serial
evaluator, so a round that fails, at whatever tuple or growth it fails,
leaves the table as a serial run does. The identity tests of section 15
assert `integer_values` and `storage_bytes` with the rest, after a
successful run, after a run a tuple limit fails, and after a run an
undefined result fails.

### 9.3 Streamed rows

A query whose plan derives each row once delivers its rows as they are
derived, `specs/design-comparison-cost.md` section 4, and no row is
delivered until nothing can fail. **A plan holding an arithmetic step can
fail on any binding, section 4.2, so a query whose plan holds one is
gathered**, into its answer relation, and delivered when the plan has run
to its end. A plan with no arithmetic step has no number slot -- an
assignment from a term binds a domain slot, section 7.2 -- so a streamed
batch never holds a pending integer, and "every batch is resolved before it
goes anywhere" needs no exception for the one destination that does not
store.

What a term assignment does change is one clause of the condition for
streaming. `specs/design-comparison-cost.md` section 4.1 streams a query
when no tuple or memory limit is set, `RELIC_F_NO_STREAM` is not, its plan
matches no argument with `_`, and its head names every variable its body
binds, so that no two bindings collapse to one row. Every clause stands,
and two change:

* a variable an **assignment** binds is a function of the variables bound
  before it, so two bindings that agree on every generator-bound variable
  agree on it too, and it need not be named for rows to be distinct. The
  head must name every variable a **generator** binds; the wildcard clause
  is what still catches `v(X, _), Y = X?` over `v(a, b). v(a, c).`, where
  every generator-bound variable is named and the row `(a, a)` would be
  derived twice, since `_` binds no variable to name;
* a plan holding an **arithmetic step** is gathered, whatever its head
  names.

A plan holding an aggregate is gathered for the same reason, whatever its
head names: a `sum` can fail on any binding and a `count` binds a number
slot, `specs/design-aggregates.md` section 9.5.

So `v(X), Y = X?` streams whether or not `Y` is an output column,
`v(X), Y = X * 2?` is gathered because it computes, `p(X, Y), Y = X?` with
only `Y` named is gathered because `X` is not, and `v(X, _), Y = X?` is
gathered because of the wildcard, as it is today.

---

## 10. Magic Sets

The rewrite of `specs/design-magic.md` reads a body in the order
`relic_plan_order()` gives it, section 5 there, and adorns each call by what
the literals before it bound. Two things about that order change and one
does not.

**An assignment binds.** The order function reports, beside the order, the
slots each placed literal binds -- what a positive literal binds, and the
variable an `ASSIGN` binds -- and the rewrite takes bound sets from that
report rather than computing "the variables of a positive literal" itself.
There is one function that decides what a literal binds at a point in a
body, and it is the planner's; the rewrite has no private notion of binding
any more than it has one of order. A call after `D1 = D + 1` whose argument
is `D1` is adorned bound, so `dist$bbf` can be asked for paths of a given
length, and a magic rule whose head is `D1` carries the assignment in its
body and is safe by construction, exactly as one whose head is a variable a
positive literal bound.

**Live variables include assigned ones.** A supplementary predicate's
columns are the prefix's live variables, those it binds that a later literal
or the head reads, and a variable an assignment in the prefix bound is one
of them. Its column then holds the integer, interned when the supplementary
tuple was stored.

**Comparisons are still appended as they are.** An assignment or a filter
is copied into the prefix, renumbered by walking its expression trees,
which is why section 6 keeps them, and hashed and compared structurally
when generated rules are deduplicated. A generated rule is a compiled rule,
and everything downstream plans and runs it without knowing that arithmetic
is in it.

**A literal's size counts its nodes.** The budgets of
`specs/design-magic.md` section 8 bound a rewrite to a constant multiple of
the program by counting literals: the program's, for the budget's size, and
those the rewrite drafts and walks, for what it spends. An expression of a
thousand nodes copied into ten adornments' prefixes is ten thousand nodes
of arena and of walking that a count of literals would not see, so every
such count becomes a count of **sizes**, a literal's size being one plus the
nodes of its expressions, which section 6 records on the node. The program's
size in literals, the literal budget, the search's charge for the literals
it walks and the count a generated rule is measured by all use it, so a
rewrite over a program of large expressions stays a constant multiple of
that program's size in nodes, and the budgets bound the work as they did.
The termination argument of section 7 there is untouched: an assignment is
one literal, and size decides cost, not what is generated.

The contract of `specs/design-magic.md` section 8, that an abandoned
rewrite answers the query from the whole program and changes nothing but
the cost, is amended by section 4.2: the whole program may meet an
undefined result the rewrite would not have reached, so the fallback may
fail where the rewrite would have answered, and never answers differently.

---

## 11. Explain

A rule is shown as written, and its expressions with the parentheses **the
tree requires**: a child of lower precedence than its parent is
parenthesized, and so is a right child of equal precedence, under every
binary operator. `D1 = D + 1`, `E > C * 2`, `X = (A + B) * C`, `-X * 2`,
`Y = 2 * (3 / 2)`, `Z = A + (B + C)`. The right-child rule is not only for
the operators that are not associative: `2 * (3 / 2)` is 2 and `2 * 3 / 2`
is 3, and `A + (B + C)` overflows where `(A + B) + C` may not, so a printer
that dropped the parentheses would show a different computation from the
one that runs. Under these rules parsing the printed text gives back the
tree that was printed, which the tests check, and two texts that mean the
same tree print the same, so a golden test does not depend on where the
writer put redundant parentheses. A negation prints as `-` before its
operand, parenthesized unless the operand is a variable.

The two operations print as

```text
filter D < 8
assign D1 = D + 1
```

and an `EMIT` prints as it does, since what a head column holds is not a
property of the plan's text. For the query `dist(a, X, D)?` against section
2's program the explain reads, under `-a`:

```text
stratum 0, component 0 (recursive): dist/3
    rule dist(X, Y, 1) :- edge(X, Y).
        scan edge(X, Y)
        emit dist(X, Y, 1)
    rule dist(X, Z, D1) :- dist(X, Y, D), edge(Y, Z), D < 8, D1 = D + 1.
        delta dist(X, Y, D):
            scan delta dist(X, Y, D)
            filter D < 8
            assign D1 = D + 1
            lookup edge(Y, Z) key={0}
            emit dist(X, Z, D1)
query $q0(X, D) :- dist(a, X, D).
    lookup dist(a, X, D) key={0}
    stream $q0(X, D)
```

Every line is printable ASCII, as `specs/design-planner.md` section 8.3
requires: an expression holds operators, integers and variable names, none
of which needs escaping, and a string constant in a comparison is escaped as
it is today.

---

## 12. Diagnostics

All follow architecture section 56: one line, lowercase, located at the
byte to look at.

Parse errors, one per statement:

```text
rules.dl:3:11: error: expected an operand, found ','
rules.dl:5:9: error: symbol foo cannot be an operand of +
rules.dl:8:40: error: expression nested more than 32 levels deep
rules.dl:12:7: error: integer literal out of range: 9223372036854775808
```

Check errors, every offender of a rule reported:

```text
rules.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal or an assignment
```

Evaluation errors, the first met in serial order, which fail the
evaluation:

```text
rules.dl:7:14: error: integer overflow in rule dist/3
rules.dl:12:9: error: division by zero in rule average/2
<query>:1:12: error: integer overflow in the query
```

An error in a rule the Magic rewrite generated, or in a semi-naive variant,
names the program rule it came from, through the origin every compiled
rule records, `specs/design-eval.md` section 2.3.

The note of verbosity level 2, once per rule, for the first comparison in
it that was false because of an operand's type, ordering or arithmetic:

```text
rules.dl:4:22: note: operand of + is a symbol; the comparison is false
```

No new warning. A variable an assignment binds and nothing reads is a
singleton and gets the singleton warning.

---

## 13. Limits and untrusted programs

| Limit | Value | Where enforced |
| --- | --- | --- |
| nesting of an expression, in the text and in the tree | 32 | `lib/parse.c`, at the production or node that would exceed it |
| distinct integers | 2^30 - 1 | `lib/value.c`, before a position is assigned |
| integer table storage | `max_memory`, with everything else | `lib/value.c`, before a growth |

The nesting bound is structural for the same reason arity's is: it sizes
the executor's cell stack and bounds the recursion of the parser and of
every walk over a tree, and a program the parser accepts can then be run
without a stack check. Thirty-two levels is more than any expression a
person writes; an expression a program generates and cannot keep within it
is split across assignments, which is what a person would do.

A program relic reads may have been written by someone who wants it to
misbehave, and arithmetic gives such a writer three new tools, each of which
this design takes away:

* **undefined behaviour.** Every arithmetic step checks before it computes,
  section 8.1, so no signed overflow, no division by zero and no
  `INT64_MIN / -1` is ever executed. The parser's descent and every tree
  walk are bounded, section 3.5, so no expression overflows the C stack,
  however it is shaped and whether or not it is well formed;
* **unbounded memory.** A rule that computes a new integer per derivation
  grows the integer table, and the table is charged to `max_memory`, section
  9.1, so `-l memory=` bounds it as it bounds a relation. A large expression
  copied by the Magic rewrite is charged to the rewrite's budgets, section
  10. Without a limit such a program runs until the tuple limit, the 2^30
  integer limit or exhaustion stops it, which is the position every
  unbounded recursive program is in today;
* **unbounded time.** A non-terminating program with arithmetic runs until a
  limit stops it, as one without arithmetic does. No step or time limit
  exists, `specs/design-api.md` section 11, and this design adds none;
  `TODO.md` records the question for 1.0.

Nothing an expression computes reaches a caller unformatted: a computed
integer is an `int64_t` and prints as one. No diagnostic quotes a computed
value, so none can carry text a program chose.

---

## 14. Interface

**The library's interface does not change.** No function, type, flag or
structure field is added: values are still symbols, strings and integers,
`relic_value_t` still carries an `int64_t`, and a computed integer in a row
is one. What changes is text:

* `relic_check()`'s description of range restriction gains "or is assigned
  by an equality whose other side is bound";
* `relic_config_t`'s description of `max_memory` names the integer table
  among what it counts;
* `RELIC_F_NOTE_MIXED`'s description names the arithmetic note beside the
  ordering one;
* `relic_materialize()` and `relic_query_run()` name an undefined arithmetic
  result as a reason to fail, beside a limit and memory, and
  `relic_query_run()` says that a query whose plan computes is gathered;
* `relic_stats_t`'s `integer_values` is described as counting the integers
  evaluation computed as well as those the program wrote.

The library's interface version is not bumped, since the binary interface
is unchanged.

**The tool does not change.** An arithmetic error is an ordinary error with
a nonzero exit, `-vv` reports the arithmetic note with the ordering note,
`-l memory=` bounds the integer table, and `-e` prints the two operations.
The manual pages describe expressions, the assignment rule, what an
expression with no value does and what an undefined result does (section
4.2), and where the termination argument now rests (section 4.6).

---

## 15. Tests

Every test is added with the change that needs it and is run against the
tree before it, per `CLAUDE.md`. Unit tests are in the C harness of
`tests/unit/`; language tests are `.dl` files with their expectations.

**Lexer.** The operator tokens; `-` as a token before a digit; `%` still a
comment after an operand; the magnitude conversion at
`9223372036854775807`, `9223372036854775808` and above.

**Parser.** Every operator and precedence, against the tree it must build;
left associativity; unary minus over a literal folding to a negative
constant, and over a variable or a parenthesis not; `INT64_MIN` as a term
and as an operand; a negative literal in a fact, a head, an argument and a
comparison, with and without space; `mod` as an operator after an operand
and as a symbol where an operand or a literal begins, including a predicate
named `mod`; the three parse errors of section 3.5 at their bytes, one per
statement; both nesting bounds met and exceeded by one in every shape --
32 and 33 redundant parentheses around one variable, 32 and 33 unary
minuses, a chain of 32 and 33 operands, `X + X * (...)` at heights 32 and
33 -- and malformed input at each depth, so that the descent bound holds
when no node is ever built; an expression in a head and in an argument
rejected; every v1 program of `tests/lang/` producing the tree it produced.

**Check.** The closure: an assignment from a positive literal's variable,
from a constant, from another assignment written after it; both sides free
reported as two offenders; a variable inside an expression on the variable
side not assigned; `_` in an assignment; the singleton warning for an
assigned variable never read; the query rule.

**Expression evaluation.** Every operator at both extremes of `int64_t`;
`INT64_MIN / -1`, `INT64_MIN mod -1`, `-INT64_MIN`, `INT64_MAX + 1`,
`INT64_MIN - 1`, `INT64_MAX * 2`, and the nearest values that fit; the sign
of `/` and `mod` for every sign combination; a symbol and a string operand
yielding no value; a program of height 32 needing 33 cells and no more, for
the left-deep, right-deep and mixed-precedence shapes. Run under
`make check-ubsan`, which must find nothing.

**Printer.** Parse, print and parse again gives the same tree for every
shape above, `2 * (3 / 2)` and `A + (B + C)` among them, and printing the
same tree from two spellings gives one text.

**Plan and executor.** An assignment placed after the literal binding its
operands, before and after its written position; a cascade of assignments;
`p(X), q(Y), X = Y` planned as a member; a number slot read by a lookup, a
member and an anti-member when its integer is interned and when it is not,
including a key with a zero column probed against an index; a batch with
pending integers measured, rewritten, committed and inserted, the
identifiers the measure promised being the ones the commit assigned; the
table's capacity after two batches of 64 new integers equal to its capacity
after one round of 128, and to its capacity after 128 single internings; a
batch the memory limit refuses at the measure leaving the table unchanged;
the error record crossing the executor's boundary and failing the
evaluation with nothing inserted from the batch; the note's mark and the
error's record kept apart, so that a rule which notes a symbol operand
under `-vv` and then overflows still fails.

**Language tests**, each a golden file:

```text
arithmetic-syntax        every operator, precedence, parentheses, negatives
arithmetic-binding       X = 42, X = foo, Y = X, Y = X + 1, in rules and queries
arithmetic-order         assignments written before their operands are bound
arithmetic-recursion     dist/3 of section 2, at one thread and with -X parallel
arithmetic-unsafe        X = Y both free; Y inside the expression; _ = X + 1
arithmetic-mixed         a symbol operand filtered out, and its -vv note
arithmetic-overflow      the error, no rows, nonzero exit
arithmetic-zero          division and remainder by zero
arithmetic-negation      section 4.2's blocked/allowed: the error, and no allowed(a)
arithmetic-strategy      section 4.2's p(1, Y)?: answered by default, failed under -X no-magic
arithmetic-depth         both nesting bounds at 32 and 33, in every shape
arithmetic-explain       -e over section 11's program
arithmetic-magic         a query binding a computed argument through the rewrite
arithmetic-magic-wide    a large expression under many adornments: budget met, fallback
arithmetic-gathered      a computing query gathered, a term-assigning one streamed,
                         and v(X, _), Y = X? gathered for its wildcard
arithmetic-memory        -l memory= refusing the integer table's growth
arithmetic-negative      offset(start, -1) and offset(start, - 1) as one fact
```

The existing `unsafe-comparison` test, whose program is `answer(X) :- X =
42.`, becomes a test of the assignment it now is, and a new unsafe program
takes its place. The explain tests whose plans change under section 7.1's
qualification are re-recorded, and the memory-limit tests at their new
totals.

**Equivalences.** `tests/equiv/strategies.sh` runs the arithmetic corpus
whose every program succeeds under every strategy -- naive, no-index,
no-reorder, no-magic, parallel -- and the answers, the tuple totals and
`integer_values` agree byte for byte, with the arithmetic note filtered
from the compared standard error as the ordering note is. A program on
which some strategy meets an undefined result is excluded from that run and
pinned by golden test per strategy, `arithmetic-strategy` above, so the
difference section 4.2 allows is exactly the one recorded. The identity at
several threads is asserted after a successful run, after a run a tuple
limit fails and after a run an undefined result fails, so that a declined
merge and an abandoned round are shown to leave the table as the serial
evaluator does.

**Generated programs.** The generator of `specs/design-hardening.md` gains
assignments and expression filters, safe by construction and defined by
construction: an assignment's operands are variables an earlier positive
literal binds and constants from a small range, its target a fresh variable
the head or a later literal reads, a divisor a nonzero constant, and the
integers a generated program states are small enough that no expression of
the shapes it generates can overflow. So no generated program meets an
undefined result, every strategy is compared on every program, and the
differential harness keeps the property that a disagreement is a bug. The
fuzz dictionary gains the operators, and the loader and evaluator targets
exercise the parse errors and the evaluation errors, which must be reported
and never crash.

**Sanitizers.** `make check-asan` over everything touching the batch, the
pending array, the map and the value table; `make check-ubsan` over the
evaluator; `make check-tsan` over the parallel identity tests.

---

## 16. Order of work

Each step lands with its tests and is measured against the one before it.

1. **Front end and program model** (sections 3, 5, 6, 11's rule printer).
   The lexer's tokens and the negative literal; expression trees with both
   nesting bounds; the closure and its diagnostic; the rule printer. Every
   v1 test holds; the new parse, check and printer tests hold. No plan runs
   an expression yet: the planner refuses a rule with an assignment or an
   expression with an internal diagnostic, so nothing is half-evaluated.
2. **Plan and executor, serial** (sections 7, 8, 9.1, 9.3, 12, 14). `ASSIGN`,
   the programs, the two kinds of slot, the read-only probe, pending
   integers with measure, rewrite and commit on the serial path, the error
   record, the gathering of computing queries, the integer table charged.
   The language and equivalence tests hold at one thread and under every
   strategy but `parallel`. Gate: `dist/3` over the chain of
   `chain-closure`, bounded at its length, within 1.5 times the unbounded
   closure's time.
3. **Magic Sets** (section 10). The order function reports bindings; the
   rewrite consumes them; renumbering and deduplication walk trees; the
   budgets count sizes. `arithmetic-magic` and `arithmetic-magic-wide` hold
   and `magic(Q, P) == Q(full_materialize(P))` holds over the arithmetic
   corpus that succeeds everywhere.
4. **The frozen round** (section 9.2). Pending integers in the candidate
   buffer, the screen by value, the measure and rewrite before the merge's
   first decline point, the commit in its reservation and append phases,
   the abandon on an error. Gate: the identity at every thread count,
   `integer_values` included, after success and after each kind of failure,
   and `dist/3` scaling as `tc-cyclic` does at sixteen threads.
5. **Hardening and documentation** (sections 13, 15). The generator, the
   fuzz dictionary and corpus, the manual pages, a bounded-distance
   workload in `bench/compare/` measured against Souffle, which has
   arithmetic, and `bench/baseline.txt` recorded again.

Steps 1 and 2 are the feature; 3 and 4 make it complete under every mode
the tool has; 5 is what `CLAUDE.md` asks of every feature.

---

## 17. Determinism

* An expression's value is a function of its operands, computed in exactly
  checked 64-bit arithmetic on every platform: no floating point, no
  implementation-defined division, no undefined overflow.
* An assignment is placed by the planner as a function of the source and
  the strategy, so which equalities bind, and where, is the same on every
  run.
* Every strategy that succeeds gives the same answer, and it is the answer
  over the integers, section 4.5. Whether a strategy meets an undefined
  result is a function of the program and the strategy, so it too is the
  same on every run; which strategies fail on a program is recorded by
  golden test, not left to vary.
* Integers computed by evaluation are interned in derivation order at their
  first stored occurrence, section 9.2, whether the round ran on one thread
  or many and whether it succeeded or failed, so `relic_stats()` reports
  the same `integer_values` at every thread count and identifiers are the
  same in every run; and, as always, nothing orders values by identifier.
* An evaluation error names the first undefined result in serial order, at
  every thread count, because it is always the serial evaluator that meets
  it.
* Which rule's type note is issued first may depend on the strategy, as it
  does today; nothing else about the diagnostics does.
* `-e` prints an expression from its tree with the parentheses the tree
  requires, so equal trees print equal and the printed text parses back to
  the tree.

---

## 18. Amendments this design makes

* `specs/architecture.md` section 6: an expression's value is a 64-bit
  integer with the operations of section 4.1; section 10: "Arithmetic
  expressions are explicitly deferred" gives way to a reference here, and
  `X = 42` is accepted, binding `X`; section 10.1: an expression with a
  non-integer operand makes its comparison false, and an undefined result
  fails the evaluation, section 4.2; section 13: the grammar of section 3.2
  and the negative literal of section 3.4; section 14: a variable may be
  bound by an assignment, section 5; section 19.2: the integer table is
  charged; section 63: the nesting limit, the integer table under the
  memory limit, and an undefined result as an ordinary error; section 66.2:
  the equivalences hold on the answers of the strategies that succeed,
  section 4.2; section 69: arithmetic expressions point here.
* `specs/decisions.md` D-11: "`=` is a filter and does not bind" stands for an
  equality whose both sides are bound; an equality with one free side and
  one bound side binds, section 4.4. D-16: a `-` is never part of a literal
  token; the parser joins the sign, section 3.4. A new decision records
  both, and records that an undefined result fails the evaluation, with
  section 4.2's reasons for and against.
* `specs/design-parser.md` section 3.2: the operator token and the integer
  token's rule; section 3.5: the sign is the parser's; section 5: a
  comparison holds two expression trees; section 9: the two nesting bounds;
  section 11: a parse tree for expressions is no longer absent.
* `specs/design-program.md` section 5: the closure of section 5 here;
  section 6: the diagnostic's wording; section 8: `relic_comparison_t`
  holds expressions, section 6 here; section 12: the reordering search
  stays absent, and the closure is why.
* `specs/design-plan.md` section 3: `ASSIGN`, section 7.4 here; section 4:
  the `NUMBER` action; section 5: the assignment rule of section 7.1;
  section 6: the second invariant's wording; section 9: "an operation for a
  comparison that binds" is no longer absent.
* `specs/design-planner.md` section 5.1: assignments placed with the tests
  and growing the bound set; section 8.2: the `assign` line and expression
  printing, section 11 here.
* `specs/design-planning-cost.md` section 3: an assignment grows the mask.
* `specs/design-eval.md` section 2.1: the `numbers` array, the cell stack,
  the pending array, the map and the error record in the scratch, sections
  8.1 to 8.4 here; section 2.3: comparisons over computed values, section
  4.3; section 3: termination is the program's, section 4.6; section 7: a
  computing query is gathered and the streaming condition of section 9.3;
  section 8: an undefined result as a third way an evaluation fails.
* `specs/design-relations.md` section 9: the integer table is counted,
  section 9.1 here.
* `specs/design-memory.md` section 8: `relic_value_find_integer()`, the
  measure, rewrite and commit of section 8.3, the table's growth to the
  stepwise capacity, and the table charged to the limit.
* `specs/design-comparison-cost.md` section 4.1: a plan with an arithmetic
  step is gathered, and the condition names generator-bound variables,
  section 9.3 here; section 7.1: the value table's growth during
  evaluation.
* `specs/design-parallel.md` section 4.4: an undefined result abandons a
  job; section 5.3: the screens for a pending column; section 6.1 and 6.3:
  the measure and rewrite before the first decline point and the commit in
  the reservation and append phases, section 9.2 here; section 7.1:
  `relic_value_find_integer()` joins `lib/frozen.h`; section 12:
  `integer_values` among the identities.
* `specs/design-magic.md` sections 5 and 6: the order function reports what
  each literal binds, and the rewrite uses it, section 10 here; section 6.1:
  renumbering walks expression trees; section 8: budgets count sizes, and
  the fallback may fail where the rewrite would have answered, sections 10
  and 4.2 here.
* `specs/design-hardening.md` section 2.4: assignments and expression
  filters generated safe and defined by construction, section 15 here;
  section 3.4: an undefined result is a failure kind the driver names.
* `specs/design-cli.md` section 9: the arithmetic note at level 2.
* `specs/design-api.md` sections 3, 5.1, 7 and 11: the text of section 14
  here; "a step or time limit" stays absent and gains a second reason to
  decide.

---

## 19. What is deliberately absent

* **Expressions in heads and atom arguments.** Section 3.3: sugar for an
  assignment, which would give the compiled rule a variable the writer did
  not write.
* **Solving for a variable inside an expression.** `X + 1 = Y` never binds
  `X`. Inverting `+` and `-` is easy, `*` and `/` are not exact, and a rule
  that inverts some operators and not others is a list.
* **Dropping the binding on an undefined result.** Section 4.2: through
  negation it derives an affirmative answer the program denies.
* **Wrapping or saturating arithmetic**, and a switch to choose it. Section
  4.2: an answer that is wrong where the reader cannot see it.
* **Failing at the end of the evaluation rather than at once**, so that
  every strategy evaluates the same bindings first. It would not: the
  strategies differ in which bindings they evaluate at all, not in when,
  and the answer would still be none.
* **Functions.** `min`, `max`, `abs`, and a call syntax for them. Each is a
  few lines once the expression machinery exists, and none is needed by a
  workload this project measures. A call in expression position would be
  unambiguous, since only comparisons hold expressions, so the grammar
  leaves room.
* **Bitwise operators.** Program-analysis workloads use them occasionally;
  the ones in `bench/` do not.
* **String operations.** Concatenation, length, substring: a different
  feature over a different type, with its own questions about the symbol
  pool.
* **Floating point.** D-4's determinism and D-3's one-identifier-per-value
  both stand against it, and no workload asks.
* **Aggregates in this design.** `count`, `sum`, `min` over a relation
  change the semantics of a rule from a conjunction to a computation over a
  set and need their own stratification rule; they are specified in
  `specs/design-aggregates.md`, which builds on the assignment, the two
  kinds of slot and the pending integers here.
* **Unboxed integer columns.** Storing a computed integer inline in a tuple
  rather than interning it would remove sections 8.3 and 9 and would be
  D-3's option B, rejected there for having two encodings of one value.
  If the interning cost of arithmetic-heavy workloads is ever measured to
  dominate, that is the decision to reopen, at the bounded cost D-3
  records.
* **Reclaiming interned integers.** Section 9.1: a count per value or a
  sweep per discard, for a saving no workload has yet shown.
* **Constant folding.** Section 3.2: no answer changes, and no measurement
  asks for it.
* **A time or step limit.** Section 13; recorded in `TODO.md` already.
* **Arithmetic in facts.** `p(1 + 1).` is a head expression, section 3.3.
