# relic aggregates design

**Status.** Design document for aggregates: `count`, `sum`, `min` and `max`
over the rows a body denotes, the literal that binds a variable to one, the
generated rule that materializes the body, and where the reader of an
aggregate sits in the stratification. Forward-looking, like every document
under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 3, 7, 9, 10.1, 11, 13, 14,
15, 30, 31, 37, 61, 63, 66, 69 and 73; `specs/design-parser.md` for the
front end this extends; `specs/design-program.md` for the compiled rule and
the safety analysis it reuses; `specs/design-arithmetic.md` for the
assignment it is shaped after, the two kinds of slot and the pending
integers it rides on; `specs/design-stratify.md` for the dependency graph
it adds an edge kind to; `specs/design-plan.md` and `specs/design-planner.md`
for the operation it adds and where the planner puts it;
`specs/design-eval.md` for the executor; `specs/design-magic.md` for what a
rewrite does with it; `specs/design-query.md` for the query that holds one;
`specs/design-parallel.md` for the frozen round; `specs/decisions.md` D-3,
D-4, D-7, D-11, D-12, D-20 and D-26.

---

## 1. What this document settles

* what an aggregate literal is, how it is written, and where it may stand;
* what it means: the body denotes a relation over its own variables, with
  the variables it shares with the rule fixed by the rule's binding, and
  the aggregate is a function of that relation -- the rows the body would
  print as a query, never the ways of satisfying it;
* the four functions, what each is worth over an empty group, over a group
  holding a value of the wrong type, and over a sum that does not fit;
* which variables of the body belong to the rule and which to the body,
  decided by where they occur and by nothing the writer has to declare;
* why the body becomes an ordinary generated rule in the program, what
  that rule is called, and why its relation *is* the group by construction;
* that a rule reads an aggregate as it reads a negated literal: from a
  strictly lower stratum, complete, so that recursion through an aggregate
  is a static error with the diagnostic recursion through negation has;
* the plan operation this adds, what it reads and binds, and why it is a
  test that binds, like an assignment, and not a generator;
* what a query holding an aggregate does with its generated rules, and
  what the Magic rewrite does with the literal;
* the diagnostics, the limits, the explain text and the tests.

It does not add recursive or monotone aggregates, `mean`, nested
aggregates, or aggregates in heads or expressions. Section 20 says why each
is left out.

---

## 2. Why aggregates, and why this much

Architecture section 69 puts aggregates second among the post-v1 features,
after arithmetic, because together they decide which programs can be
written at all. A relation can be joined, filtered and negated, and with
arithmetic its values can be computed with; what it cannot be is
**measured**. Without aggregates a program cannot ask how many, the total,
the largest or the smallest, which is half of what people ask of
relations:

```prolog
% How many dependencies each task has, zero for a task with none.
dep_count(T, N) :-
    task(T),
    N = count { depends(T, D) }.

% A task is ready when none of its dependencies is unfinished.
ready(T) :-
    task(T),
    N = count { depends(T, D), not done(D) },
    N = 0.

% The total size of each directory's files.
dir_size(Dir, S) :-
    dir(Dir),
    S = sum(Size) { file(F, Dir), size(F, Size) }.

% The shortest distance between two nodes, over the bounded dist/3 of
% the arithmetic design, which enumerates every path of length below 8.
shortest(X, Y, M) :-
    node(X), node(Y),
    M = min(D) { dist(X, Y, D) }.
```

Every example reads a relation that is complete before the rule runs,
groups it by values the rule has already bound, and folds each group into
one value the rule then tests or emits. That is the feature: **an equality
whose right side is `count`, `sum`, `min` or `max` over a body binds its
left side to the aggregate of the rows that body denotes, for the values
of the variables it shares with the rule.** Nothing else about the language
changes. Heads and atom arguments still hold terms, values are still
symbols, strings and integers, a rule is still a conjunction of literals,
and a relation an aggregate reads is complete when it is read.

Pure Datalog has no aggregates, as it has no negation, no comparisons and
no arithmetic. Each is an extension, and stratified aggregation is the one
that has been understood since 1990: the aggregated relation must be
complete before the fold, so its readers sit in a higher stratum, exactly
as a negated relation's do. The design takes that treatment and nothing
more. Architecture section 71 asks for no feature without a clear Datalog
use, and that is why there are four functions, why a body may not hold an
aggregate, and why an aggregate over a relation that depends on its reader
is an error rather than a semantics: each of the things left out has a use
somewhere, none has one that the programs of architecture section 4 or the
workloads of `specs/design-bench.md` need, and section 20 leaves room for
each rather than deciding it now.

---

## 3. Syntax

### 3.1 Tokens

Two marks join the punctuation of `specs/design-parser.md` section 3.2:

```text
{ }             punctuation
```

Nothing else changes in the lexer. `count`, `sum`, `min` and `max` are
ordinary symbol tokens, recognized as function names by the parser **only
in the one position where a function may stand**, after the `=` of an
equality and followed by `(` or `{`, exactly as `not` is recognized only
where a literal may begin and `mod` only after an operand. A symbol in
that position that is not followed by `(` or `{` is the constant it always
was, so `status(T, count).` stores the symbol `count`, `X = count` assigns
it, and a predicate may be called `count`, `sum`, `min` or `max`, since at
the start of a literal the word is a name as it always was.

### 3.2 The aggregate literal

```text
literal      := atom
              | "not" atom
              | comparison
              | aggregate

aggregate    := variable "=" function "{" group "}"
function     := "count"
              | ("sum" | "min" | "max") "(" variable ")"
group        := literal ("," literal)*
```

The variable before `=` is the **result**, the variable in parentheses the
**aggregated variable**, and the literals in braces the **group body**. The
group body is a rule body: positive literals, negated literals, comparisons
and assignments, in any order, with `_` where a positive or a negated
literal admits it, `specs/design-negation.md` section 2. The one production
it does not admit is `aggregate` itself; section 3.4 refuses the nesting,
and section 20 says why.

`count` takes no variable, because the cardinality of a set of rows needs
no column named; `sum`, `min` and `max` take exactly one, the column they
fold. The parentheses are what make the aggregated variable read as an
argument rather than as a term that follows the function, and they are
decidable by one token of lookahead: in operand position a symbol is never
legally followed by `(` or `{`, so the parser, holding the symbol, looks at
the token after it and knows.

### 3.3 Where an aggregate may stand

**Only as the whole right side of an equality whose left side is a bare
variable.** So

```prolog
p(X, N) :- q(X), N = count { r(X, Y) }.            % accepted
p(X) :- q(X), count { r(X, Y) } > 3.                % rejected
p(X, M) :- q(X), M = count { r(X, Y) } + 1.         % rejected
p(X, count { r(X, Y) }) :- q(X).                    % rejected
p(X, N) :- q(X), N = count { r(X, Y), M = count { s(Y, Z) } }.  % rejected
```

and each is written as the accepted form plus a comparison or an
assignment: `N = count { r(X, Y) }, N > 3`, and `M = N + 1`. The
restriction is the one `specs/design-arithmetic.md` section 3.3 gives for
expressions in heads, applied twice over. An aggregate inside an expression
would be a subtree that reads a relation, which no expression does, and
`specs/design-arithmetic.md` section 8.1's postfix programs are built on
every step reading the frame and nothing else; an aggregate in a head would
be sugar for a fresh variable and an aggregate literal in the body, and the
compiled rule would hold a variable the writer never wrote. The equality
form says what the program means in the vocabulary the assignment already
established: **`V = ...` binds `V` to a value the body computes**, and
whether the right side is an expression or an aggregate is the only
difference.

### 3.4 What is checked while parsing

The parser owns four checks an aggregate can fail, each a diagnostic at the
byte the reader should look at and each subject to the one-message-per-
statement rule of `specs/design-parser.md` section 7:

* **a function stands somewhere other than after `V =`.** The parser meets
  a function name followed by `(` or `{` while parsing the right operand
  of a comparison. When the left operand is not a bare variable, or the
  comparator is not `=`, the message says what an aggregate looks like:
  `an aggregate is written V = count { ... }`. A `{` anywhere else is the
  ordinary unexpected token.
* **the function's variable is wrong.** `count(` is refused with `count
  takes no variable`; `sum {`, `min {` and `max {` with `sum takes one
  variable in parentheses`; and a constant or an expression in the
  parentheses with `expected a variable`.
* **a group body holds an aggregate.** Refused where its function is
  written, with `an aggregate body cannot hold an aggregate`. This is the
  check that bounds the parser's descent: the `group` production is
  entered only from `aggregate`, and an `aggregate` is refused before its
  `group` is entered when one is already open, so the parser recurses at
  most one level however the text is shaped. Section 14 says why the
  bound is structural.
* **the body is empty**, `count { }`: the ordinary `expected a literal,
  found '}'`.

An aggregate that parses is one more kind of literal in the tree of
`specs/design-parser.md` section 5: the function, the aggregated variable
when there is one, the result variable, and a body that is an array of
literals as a rule's is. The tree is a transcription of the text, as the
rest of the syntax tree is, and it is what `-e` prints a rule from. Every
program v1 accepts parses to the same tree: `{` and `}` were errors, and a
function name followed by `(` or `{` in operand position was an error, so
what this design accepts is new text and no existing program changes
meaning.

---

## 4. Meaning

### 4.1 The group

Take a rule holding an aggregate literal, and call the rest of the rule
the **outer rule**. Some variables of the group body also occur in the
outer rule; call those **shared**. The others occur only inside the braces;
call those **local**. Section 4.2 gives the exact rule; the meaning rests on
the distinction alone.

> Fix a binding of the shared variables. The **group** is the set of
> bindings of the local named variables under which the group body holds,
> in the model of the strata below the rule's. The aggregate is a function
> of the group and of nothing else.

The group is a **set of rows over the named local variables**, and it is
exactly what the body would print as a query with the shared variables
substituted: `_` contributes no column, a constant contributes no column,
and two ways of satisfying the body that agree on every named local
variable are one row. That is the query semantics of architecture section
11, applied inside the braces. So over `edge(a, b). edge(a, c).`:

```prolog
n(N) :- N = count { edge(a, Y) }.     % n(2): two rows, Y = b and Y = c
n(N) :- N = count { edge(a, _) }.     % n(1): one row, with no columns
```

The second is not a mistake the language makes; it is what `edge(a, _)?`
prints, one empty row, and the same rule read the same way. The out-degree
of a node is the number of its distinct neighbours, and the neighbour is
named. The manual page says so where it introduces `count`, with this
example, because it is the one place the set reading and the intuition of
SQL's `COUNT(*)` part ways, and a reader coming from SQL will meet it
first.

The alternative, counting every way of satisfying the body, was
considered and rejected. It would make `sum(Y) { p(X, Y), r(X, _) }`
multiply each `Y` by the number of `r` tuples for `X`, a multiplicity the
language cannot otherwise observe: no query can show it, no relation holds
it, and D-3's set semantics has no place to keep it. The set reading is
the one definition that is stated in terms the language already has, and
it is what lets section 5 materialize the body as an ordinary rule whose
relation is the group by construction.

### 4.2 Shared and local variables

Architecture section 7 gives a variable one scope, the rule. An aggregate
body narrows that for the variables that are its own:

> A variable belongs to a group body when every occurrence of it in the
> statement lies inside that body's braces; it is **local** to the body.
> Every other variable belongs to the rule, and one that occurs in a group
> body is **shared** with it.

Two consequences follow from stating the rule by occurrence. A variable
that occurs in two group bodies and nowhere else is shared with both, and
must be bound by the outer rule as any shared variable must; the
alternative, two variables of one name in one rule, would be the first
place in the language where a spelling names two things. And **the
aggregated variable is an occurrence inside the body**: `sum(Size)` names a
column of the group, so `Size` is local in `S = sum(Size) { file(F, Dir),
size(F, Size) }`, as it must be for the rule to mean anything. The result
variable is an occurrence in the outer rule.

The rule needs no declaration, no `^` and no grouping list. What groups is
what the rule already bound, which is what a reader expects and what the
older explicit forms made the writer say twice.

### 4.3 The four functions

| Function | Value over a group | Over an empty group | Result |
| --- | --- | --- | --- |
| `count` | the number of rows | 0 | a computed integer |
| `sum(A)` | the exact sum of the rows' `A` | 0 | a computed integer |
| `min(A)` | the least `A`, in the ordering of its type | no value | a value of the group |
| `max(A)` | the greatest `A` | no value | a value of the group |

`count` and `sum` are total: every group has a count and a sum, the empty
group's being zero, which is what makes `ready/1` of section 2 hold for a
task with no dependencies and `dep_count/2` say zero rather than nothing.
`min` and `max` are partial: an empty group has no least element, and
**an aggregate with no value makes its literal false**, so the binding is
dropped as a comparison drops one. A rule that wants a default writes it,
as it writes one for a missing tuple:

```prolog
best(X, M) :- node(X), M = max(S) { score(X, S) }.
best(X, 0) :- node(X), N = count { score(X, S) }, N = 0.
```

`min` and `max` order by the rule of architecture section 10.1, within a
type: integers numerically, symbols and strings bytewise. A group whose
`A` column holds one type has a least and a greatest element; `min(N) {
name(P, N) }` is the first name in byte order, which is a defined and
useful answer. A group whose `A` column mixes types has no order and is
section 4.4's case.

`count` cannot fail, and cannot overflow: a group is a relation, and a
relation holds fewer than 2^63 tuples. `sum` is exact over the group and
fails only when the total does not fit, section 4.5.

### 4.4 A group the function has no value over

Two things can put a group outside a function's domain, and both are
answered as `specs/design-arithmetic.md` section 4.2 answers a symbol
operand of `+`:

* **`sum` meets a row whose `A` is a symbol or a string;**
* **`min` or `max` meets rows whose `A` values are of two types.**

In both **the aggregate has no value and the literal is false**, the
binding is dropped, and the note of verbosity level 2,
`RELIC_F_NOTE_MIXED`, reports it once per rule as it reports a cross-type
ordering and a symbol operand:

```text
sizes.dl:4:22: note: a row of sum(Size) holds a value that is not an integer; the aggregate has no value
names.dl:7:19: note: the rows of min(N) hold values of more than one type; the aggregate has no value
```

Neither note names the types it met. Which row a fold meets first depends
on the order the group's rows were inserted in, which section 4.5 lets
differ between strategies, and a note that named the first offending pair
would then differ in wording between them; a note that says only what is
true of the whole group is the same under every strategy, and section 18
can keep the promise that nothing about a diagnostic but the order of the
notes depends on the strategy.

This is a defined answer over the language's three types, not a missing
one. Predicate arguments are untyped, and architecture section 10.1 decided
that a column holding a symbol beside an integer is a fact about the data
rather than a mistake; a total over a column with a symbol in it is not an
integer, as a symbol plus one is not, and the rule that reads it is
genuinely false in the three-typed language. The argument of
`specs/design-arithmetic.md` section 4.2 that a *dropped* binding is
dangerous through negation applies to a lost integer, not to a value that
was never one; the type case there drops the binding for the same reason
this one does.

### 4.5 A sum that does not fit

A `sum` whose total does not fit `int64_t` is **an evaluation error**, the
one `specs/design-arithmetic.md` section 4.2 gives an overflowing `+`, with
its reasons: dropping the binding would, through negation, derive an
affirmative answer the program denies, and wrapping would derive a false
total silently. The evaluation stops as a limit stops it -- no rows, a
nonzero exit, the relations stale -- and the diagnostic names the rule:

```text
sizes.dl:9:14: error: integer overflow in rule dir_size/2
```

**The sum overflows exactly when the total over the integers does not fit,
whatever order the rows are visited in.** This is stronger than what `+`
promises, and it has to be. An expression is a tree, and the order of its
additions is the order the writer gave it, the same on every run; a group
has no order the language can see, and the order its rows are visited in
is the order they were inserted, which `specs/design-eval.md` section 10
fixes per strategy and not across them. A fold that added in `int64_t` and
checked each step would overflow on `INT64_MAX, 1, -1` and not on `1, -1,
INT64_MAX`, and whether a program failed would then depend on which
strategy inserted the group's tuples in which order -- a difference the
equivalences of architecture section 66.2 could not state and the
differential harness could not test. So the fold accumulates exactly,
section 9.2, and asks once, at the end, whether the total fits. A sum then
fails as a function of the group's *set* of rows, and the contract of
`specs/design-arithmetic.md` section 4.2 -- a strategy that evaluates more
bindings may fail where one that evaluates fewer succeeds, and every
strategy that succeeds gives the answer over the integers -- holds for
aggregates with no further amendment: which groups are folded depends on
the bindings a strategy reaches, and what each fold gives does not.

### 4.6 The aggregate binds, and why the order of binding does not matter

The aggregate literal is an assignment in the sense of
`specs/design-arithmetic.md` section 4.4: **an equality whose left side is
a variable nothing before it has bound, and whose right side is an
aggregate every shared variable of which is bound, binds that variable to
the aggregate's value.** "Before it" is the plan's order and "bound" is
what the literals placed before it bound, as there. When the plan has
bound the result variable before the literal is placed -- by a positive
literal, by an assignment or by another aggregate -- the equality is a
filter, as an equality over two bound sides is, and the aggregate's value
is compared against the binding: `p(X, N) :- q(X, N), N = count { r(X, Y)
}` holds for the `N` of `q` that is the count, and `same(N) :- N = 1 + 1, N
= count { p(X) }` for a count of two. Whether the literal binds or compares
is a fact about the plan, decided where the order is, as the kind of a
slot is, `specs/design-arithmetic.md` section 7.2; the answer is the same
either way, by the argument below.

The result is a function of the shared variables' values and of the group
relation, which is complete when the literal runs (section 7), so the
argument of `specs/design-arithmetic.md` section 4.5 extends to it
unchanged: every schedule the plan may choose produces, for a fixed tuple
per positive literal, exactly the bindings that satisfy every comparison
and every aggregate literal, or meets a sum that does not fit. A binding an
aggregate literal produced satisfies it by construction; a satisfying
binding gives the result variable the one value the fold computes, since
the fold is a function; and section 4.5 makes whether the fold fails a
function of the same inputs. So every strategy that succeeds derives the
same tuples, and the planner may place an aggregate literal wherever its
shared slots are bound.

### 4.7 Recursion through an aggregate is rejected

```prolog
shortest(X, Y, M) :- edge(X, Y), M = min(D) { shortest(X, Y, D) }.
```

is rejected, as a rule that negates a relation depending on its own head
is. Under a least fixed point the minimum over a relation still growing is
not a fact about the relation, and the semantics that admit such rules --
monotone aggregation over a lattice, or a well-founded reading -- are the
kind of second theory architecture section 73 rules out. The stratification
rule of section 7 says so statically, with the diagnostic recursion through
negation has, and the program is written in two strata: a bounded
recursive relation, as `dist/3` of `specs/design-arithmetic.md` section 2,
and the aggregate over it once it is complete, as `shortest/3` of section 2
here.

---

## 5. The group rule

### 5.1 The body is a rule

An aggregate literal's body is compiled into a **group rule**, an ordinary
rule the program's rule table holds, whose head is a generated **group
predicate** over the body's shared and local variables:

```prolog
dir_size(Dir, S) :- dir(Dir), S = sum(Size) { file(F, Dir), size(F, Size) }.

agg$dir_size$2$0$2(Dir, F, Size) :- file(F, Dir), size(F, Size).
```

The head's columns are the **shared variables**, in the outer rule's slot
order, followed by the **local named variables**, in order of first
occurrence in the body. The group predicate's relation then holds, for
every binding of the shared columns, exactly the rows of section 4.1 under
that binding: its tuples with that prefix, distinct because a relation is a
set, and with no column for `_` because a wildcard has no slot. **The
relation is the group by construction**, and the aggregate literal in the
outer rule is a fold over the tuples that share its key.

This is architecture section 71's "transform before complicating the
evaluator", and it is why the design needs no new state in the executor.
The alternative, evaluating the body afresh for each binding of the outer
rule with the shared slots pre-bound, was considered and rejected: the
executor would gain a nested plan, a scratch set to make the local
bindings distinct, memory that grows with the group and needs an account
of its own, and a body re-evaluated once per binding; the naive oracle,
the Magic rewrite and the frozen round would each need a case for it. The
group rule gets all of that from machinery that exists -- duplicates
collapse on insertion, the tuple and memory limits count the relation, the
schedule orders it, `-e` shows it, a parallel round shares its pass -- at
the cost of storing the body's relation once, which the memory limit
bounds as it bounds any relation. One body shape pays that cost for
nothing: a body that is already a relation, section 5.6, is folded in
place and gets no rule.

### 5.2 Names

The group predicate is named by D-12's rule for generated names, from
positions and never from a counter:

| Predicate | Name | Arity |
| --- | --- | --- |
| the group of the aggregate that is the 2nd literal of the 1st rule deriving `dir_size/2` | `agg$dir_size$2$0$2` | shared + local variables |
| the group of the aggregate that is the 3rd literal of a query | `agg$$q0$3` | likewise |

The name carries the source predicate's name and **arity**, the rule's
position from 0 among the rules deriving that predicate, in program order,
and the literal's position from 1 among the outer body's literals, in
written order, as `specs/design-magic.md` section 4 names a supplementary
predicate by its rule and its call. The arity is in the name because a
predicate is `(name, arity)` and nothing else, `specs/design-program.md`
section 3: `p/1` and `p/2` are unrelated predicates whose first rules may
each hold an aggregate at the same position over bodies of the same width,
and a name without the arity would give the two groups one relation. An
adorned name needs no arity because its adornment is as long as the arity.
Rules are only ever appended, so a position never shifts.

A query's group predicate is named after `$q0` whatever the query's serial,
as a supplementary predicate cut from a query's rule is, so that a later
query with an aggregate at the same position and of the same arity reuses
the predicate entry, and one of another arity gets a predicate of the same
name and another arity. The entry is all that is shared: the relation is
emptied when a run ends, section 10, and which group rule the entry stands
for is a fact about the literal that reads it, section 6, never about the
entry. The name holds `$`, which no identifier the lexer accepts contains,
so no program can name, read or write a group predicate, and the
undefined-predicate warning never mentions one.

The group predicate enters the context's predicate table as every
generated predicate does, `specs/design-program.md` section 3, interned
when the aggregate literal is reached in the body walk and before the
atoms inside its braces, so identifiers keep first-appearance order. It is
marked `head_named` when generated and `derived` when its rule is
appended; it has no base store, since nothing can state a fact for it. It
records the predicate whose rule holds the aggregate, none for a query's,
which is what a diagnostic names it by, section 14.

### 5.3 Safety

`specs/design-program.md` section 5 states range restriction as one set
computation, and `specs/design-arithmetic.md` section 5 grows it to a
closure over assignments. An aggregate is checked by that closure twice,
in two rules, with no third computation:

**The group rule is checked as a rule.** Its head names every shared and
every local named variable, so the ordinary check asks whether each is
bound by a positive literal or an assignment of the body. That is the one
restriction this design places on a shared variable: **it must be bound
inside the braces as well as outside them.** Otherwise the group relation
would need a column the body cannot fill:

```prolog
above(N) :- threshold(T), N = count { score(P, S), S > T }.       % rejected
above(N) :- threshold(T), N = count { score(P, S), threshold(T), S > T }.   % accepted
```

The accepted form names, inside the braces, the relation that gives `T` its
values, and groups by `T`; it is what the engine would have to do anyway,
and the diagnostic says so:

```text
scores.dl:3:56: error: variable T in the aggregate of rule above/1 is not bound by a positive literal or an assignment of the aggregate body
scores.dl:3:56: note: a variable bound outside an aggregate must also be bound inside it
```

The alternative, supplying the outer rule's bindings to the group rule
through a prefix relation as the Magic rewrite's supplementary predicates
do, is correct and general, and section 20 leaves it out until a program
shows the restriction biting: it would evaluate every outer prefix twice
or materialize one more relation per aggregate, for a shape of rule the
idiom above already expresses.

**The outer rule is checked with the aggregate as an assignment.** The
closure of `specs/design-arithmetic.md` section 5 gains one clause beside
the equality's:

```text
B = slots of every positive literal
repeat until B does not change:
	for each equality, in written order, with one side a bare
	    variable v not in B and every slot of the other side in B:
		add v to B
	for each aggregate literal, in written order, whose result
	    variable v is not in B and whose every shared slot is in B:
		add v to B
R = slots of the head, of every negated literal, of every comparison,
    and the shared slots and the result slot of every aggregate literal
safe exactly when R \ B = {}
```

The shared slots are required slots, as a negated literal's are: the fold
needs them bound. The result slot is required too, and the closure is what
binds it, so a result variable the outer rule never binds otherwise is
safe exactly when the shared variables are. Two consequences fall out with
no case of their own. A result variable that also occurs inside its own
braces, `N = count { p(N) }`, is shared, so the literal binds `N` only once
`N` is bound, which nothing does: `N` is reported unsafe at its first
occurrence. And a shared variable the outer rule binds nowhere -- one that
occurs in two bodies and no positive literal, or only in a comparison -- is
reported by the ordinary message at its first occurrence, which may lie
inside the braces:

```text
graph.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal or an assignment
```

The closure is monotone and adds a slot or ends on every pass, so it makes
at most 64 passes, as before, and the planner's placement of section 8.1
is guaranteed by the same argument that guarantees an assignment's: once
every positive literal is placed the bound set contains the closure's *B*,
so every aggregate the closure fired can fire.

### 5.4 Numbering, singletons and the arity limit

The outer rule's variables are numbered as `specs/design-program.md`
section 4 numbers them, in order of first occurrence reading the head and
then the body in textual order, over every occurrence that is not local to
a group body; a shared variable's first occurrence may lie inside the
braces, and it takes its slot there. The group rule's variables are
numbered by the same rule over the group rule, head first, so the shared
columns come first in outer slot order and the local ones after them in
body order. Local variables take no slot of the outer rule, so a rule with
several aggregates over wide bodies does not approach the limit of 64 for
variables it never reads.

Deciding which variables are local needs the whole statement, since a
later literal can share a variable an earlier body mentions. The compiler
therefore reads the statement's occurrences once to classify them and then
builds the outer rule and its group rules in a second pass; the tree is
in the arena, and the statement's length bounds both passes. This is the
one place the one-pass build of `specs/design-program.md` section 8.1
becomes two, and the reason is the same as the reason for the closure: the
answer depends on text that comes later.

**No singleton warning arises from inside a group body.** Every named
local variable is a column of the group rule's head, so it occurs at least
twice in the rule the body compiles to, as a query's output variable does;
`count { edge(X, Y) }` names `Y` once in the text and twice in the rule the
text means, and it is not a typo but the column that makes rows distinct.
The outer rule's own singletons are reported as today: a result variable
the rule never reads, `p(X) :- q(X), N = count { r(X, Y) }`, is mentioned
once and draws the warning, which is the aggregate form of the typo the
warning exists for.

**A group predicate has at most 32 columns**, since it is a predicate, per
D-7. A body whose shared and local named variables together exceed 32 is
rejected at the occurrence that would be the 33rd:

```text
wide.dl:12:207: error: the aggregate in rule wide/1 mentions more than 32 variables
```

### 5.5 When the rules are appended

A statement holding aggregates compiles to one outer rule and one group
rule per aggregate, and **nothing is appended until the whole statement
has passed**. `specs/design-program.md` section 8.1 discards an unsafe
statement rather than appending it, so that nothing incomplete is
reachable; a group rule appended before its outer rule was found unsafe
would be a rule deriving a relation nothing reads, and a load that failed
between two appends would leave half a statement in the table for the
retry to duplicate. So the compiler builds every rule of the statement into
the arena, checks each, and then reserves room in the rule table for all
of them at once -- the one step that can fail, for memory, and one that
fails leaves the table as it was and the cursor on the statement -- and
appends them in order, group rules first and the outer rule last, through
the one infallible append that marks each head derived. A rejected
statement appends nothing, and its group predicates, already interned,
are referenced by nothing and named by nothing, as a predicate interned by
an abandoned rewrite is.

The group rules of a program's statement are program rules: they count in
the pair of `specs/design-eval.md` section 6, they are in the dependency
graph and the schedule, and `relic_materialize()` evaluates them. A
query's are the query's, section 10.

### 5.6 A body that is already a relation

The commonest aggregate body is one atom:

```prolog
outdeg(X, N) :- node(X), N = count { edge(X, Y) }.
total(N) :- N = count { valueAlias(A, B) }.
```

Its group rule is `agg$outdeg$2$0$2(X, Y) :- edge(X, Y).`, a rule that
copies `edge` into a second relation column for column and then indexes
the copy on `X`. The copy is exact and useless: when every argument of the
one atom is a distinct named variable, the body denotes the atom's
relation itself, up to the order of its columns, so the group of section
4.1 under a binding of the shared variables is the set of the atom's
tuples that hold that binding in the columns the shared variables stand
in. Nothing about the fold needs the copy. It needs a relation to walk,
the columns of that relation its key is matched against, and the column
it aggregates, and the atom's relation supplies all three.

**Such a body is folded in place.** A body is a relation when it is
exactly one positive literal whose arguments are pairwise distinct named
variables -- no constant, no `_`, no variable twice, and no comparison,
assignment or negated literal beside it. A nullary atom qualifies, its
zero arguments being distinct. For that body the compiler generates no
group predicate and no group rule. The aggregate literal reads the atom's
predicate as the relation it folds, its **key columns** are the atom's
columns that hold shared variables, each bound from the outer slot of its
variable, and its aggregated column is the column of the aggregated
variable:

```text
N = count { edge(X, Y) }        X shared, Y local:   fold edge, key column 0 from X's slot
N = count { edge(Y, X) }        X shared, Y local:   fold edge, key column 1 from X's slot
S = sum(W) { weight(X, Y, W) }  X and Y shared:      fold weight, key columns 0 and 1, aggregated column 2
N = count { valueAlias(A, B) }  nothing shared:      fold valueAlias whole, no key, no index
```

The key is no longer a prefix of the relation's columns, as a group
relation's is by section 5.4, but a set of them, and the planner and the
executor already handle sets: a positive literal with some arguments bound
is looked up through the index keyed on exactly those columns, whatever
their positions, `specs/design-planner.md` section 5.2, and an index probe
is laid out as a tuple of the relation with only the key columns read,
`specs/design-index.md`. The fold uses the same index and the same probe,
section 8.3. With no shared variable it walks the relation and builds
nothing at all, which is the `valueAlias` case above, where the copy was
the whole of the cost.

**The identity is exact, so the answers do not change.** A relation is a
set, and the group rule's relation is the atom's relation under a
permutation of columns, so every function gives the same value over the
same rows, and section 4.5's independence of row order makes the walk's
order irrelevant. Stratification gives the reader the same stratum: the
aggregate edge, section 7, now points at the atom's predicate instead of a
group predicate whose one positive edge pointed there, and a strict edge
followed by a positive one places its source where one strict edge to the
positive edge's target does. Magic Sets marks the atom's predicate
complete, as it marked the group predicate and everything its body read,
section 11. The frozen round reads a complete relation, as it did.

What does change is what surrounds the answer, and each difference is one
the two forms are entitled to. `-e` shows the operation over the atom's
relation and no group rule, section 12. The tuple count no longer includes
a copy, so a `-l tuples=` limit a copy would have passed is not reached,
and a `-l memory=` limit that refuses the fold's index names the relation
indexed rather than an aggregate derived, section 14. An unstratified
program's cycle is found by a breadth-first search, `specs/design-stratify.md`
section 5, and an aggregate that is one edge rather than two can make a
different, equally valid, cycle the shortest, so the notes may differ. And
two aggregates of two rules that are structurally equal, which the Magic
rewrite's deduplication, section 11, told apart only by their distinct
group predicates, now fold one predicate with one key and merge, so a note
about them is issued once and names the surviving rule's variables; the
answers are the same.

**The key columns are in column order.** `shared[i]` is the outer slot of
the `i`th key column counted from the lowest, section 6, and not the
`i`th shared variable in the outer rule's slot order, which is what a group
rule's leading columns happen to coincide with. `S = sum(W) { w(Y, X, W) }`
with `X` in slot 0 and `Y` in slot 1 has key columns 0 and 1 bound from
slots 1 and 0, in that order; filling `shared` in slot order would fold
the wrong groups silently, which section 16 tests for.

**What the compiler keeps of the body.** The aggregate literal still
carries the body as compiled, section 6, so that `-e` prints the braces as
they were written, the Magic walk visits what the body reads, and a note
names the aggregated variable. For a body folded in place that compiled
body is a `relic_rule_t` with no head: `head` is `RELIC_ID_NONE`,
`head_arity` is zero and `head_args` is `NULL`, which is less than a
query's rule has, a query's head being none but its output columns being
head arguments; `origin`, which only the notes of a rule being evaluated
read, is never read, since the body is never evaluated; the body is the
one literal, and its variables are numbered in the order the
atom's arguments are written, so that **its slot `c` is column `c` of the
relation the fold reads**, which is the invariant section 6 states for a
group rule's variables too. The aggregated column is found among those
variables by name, as a group rule's is found among its head columns. The
body is never published to a rule table, never derives anything and is in
no graph; it is a description, held by the literal and nothing else, in
the arena the literal lives in. The group predicate's name of section 5.2
is not interned for it: a predicate nothing derives and nothing reads
would be a table entry with no purpose.

The alternative was the identity rule, `edge(X, Y) :- edge(X, Y)`, as the
body of a fold in place: then the predicate a literal folds would always be
its body's head, one field fewer. It was rejected because a rule whose head
is a predicate the program wrote, held where rules are held, says "this
derives `edge`" to every reader that does not know the convention, and
the one field it saves is the one that makes the convention unnecessary. A
body with no head cannot be mistaken for a derivation.

**Why `source` and `key_mask` are stored.** Both follow from the body: a
group rule's head is what it derives and its key is its leading columns,
and a fold in place reads its one atom's predicate through the columns its
shared variables stand in. Storing them spares every reader -- the
dependency graph, the planner, the walk, the hash -- the two-way question
of which body it holds, so that the two forms are one to everything but
the compiler that makes them and the printer that shows them.

**What does not qualify, and why it stays a rule.** `count { edge(X, _) }`
is a projection: its group holds the distinct values of `X`, which `edge`
does not. `count { edge(X, a) }` is a selection, and `count { edge(X, X) }`
too. Each is a different relation from the atom's and needs the rule that
computes it. A selection by a constant could be folded in place through an
index keyed on the shared columns and the constant's column, with the
constant in the probe; a projection under `min` or `max` could too, since
the extreme of a projection is the extreme of the relation; section 20
leaves both out until a workload shows them, because each is a second way
to build a key and the one atom of distinct variables is the shape every
degree count, total and extreme over a relation is written in.

**For testing, the rule can be forced.** `RELIC_F_GROUP_RULES`, spelled
`-X group-rules`, compiles every body to a group rule as sections 5.1 to
5.5 describe, folding none in place. It is a strategy in the sense of
`specs/design-hardening.md` section 3.1: its answers must equal the
default's over every program, except where a limit refuses one of them,
and the generator there, whose bodies are drawn at random, produces
single-atom bodies often enough that the column map of every fold is
checked against the copy it replaces. It evaluates more rules than the
default, so its tuple total is held to nothing, as `no-reorder`'s and
`no-magic`'s are not, and `tests/equiv/strategies.sh` compares its
answers and exit status but not its standard error, which the limit
messages and the cycle notes above may change.

The flag has a second user that settles whether it is worth a public
flag: the random tester's derivation oracle, `tests/random/compare.c`,
counts one set of derivations per group body, and under this design a body
that is a relation derives nothing. Rather than give the tester a copy of
the compiler's test for a relation -- a second statement of one rule, which
would drift -- the oracle runs under `RELIC_F_GROUP_RULES`, where every
body derives, and the equivalence runs compare the default against it. The
alternative of rendering each single-atom body twice, once doubled as
`count { edge(X, Y), edge(X, Y) }`, which the same relation denotes and
which no rule folds in place, would check the column map without a flag,
but would still leave the oracle needing to know which bodies derive.

---

## 6. The compiled rule

The literal of `specs/design-program.md` section 8 gains a fourth kind
beside the positive atom, the negated atom and the comparison:

```c
enum relic_aggregate_fn {
	RELIC_AGG_COUNT,
	RELIC_AGG_SUM,
	RELIC_AGG_MIN,
	RELIC_AGG_MAX
};

struct relic_aggregate {
	const uint16_t		*shared;	/* outer slot of each key column, in column order */
	const relic_rule_t	*body;		/* the body as compiled, a copy in the owner's arena */
	relic_location_t	 where;		/* the function name */
	relic_pred_id_t		 source;	/* the predicate whose relation the fold reads */
	uint32_t		 key_mask;	/* the source's columns the key is matched against */
	uint16_t		 shared_count;	/* the key columns, popcount(key_mask) */
	uint16_t		 column;	/* the aggregated column; unused by COUNT */
	uint16_t		 result;	/* the slot the literal binds */
	enum relic_aggregate_fn	 fn;
};
```

The literal names what it folds and carries what it was written as, and
the two are distinct fields because they coincide for one body shape and
not for the other.

**What it folds** is the relation of `source`, reached through the
predicate as every relation is: the group predicate when the body was
compiled to a group rule, and the atom's own predicate when the body is
one atom folded in place, section 5.6. The fold visits the tuples whose
`key_mask` columns hold the key, and `shared[i]` is the slot of the outer
rule the `i`th key column, in increasing column order, is bound from; for
a group relation `key_mask` is the leading `shared_count` columns, by
section 5.4, and for an atom folded in place it is the columns its shared
variables stand in. `shared` lives in the arena that holds the literal,
beside the rule's arguments. The aggregated column is a column index into the
source relation, resolved when the rule is compiled, so nothing downstream
looks a variable up by name.

**What it was written as** is `body`, the rule the braces compiled to,
through a pointer to a copy made in the same arena as the literal: the
program arena for a program statement, the query's own arena for a query,
section 10. For a body compiled to a group rule the copy is the group rule;
for one folded in place it is the headless one-literal rule of section 5.6,
the only instance there is. In both the variables are numbered so that
**slot `c` is column `c` of the source relation**, which is what lets the
printer name the relation's columns and the aggregated column from the
body alone. The pointer is not into a rule table: a table is a growable
array, and a pointer into one dangles when a load grows it,
`specs/design-program.md` section 6. The copy is never moved or changed,
and a group rule copy's arguments, body and variables are the arena
blocks the table's entry holds too, so it lives as long as the literal
does and costs one rule header. A copy of the literal the Magic rewrite
makes keeps the pointer, so the printer of section 12, the walk of section
11 and the notes of section 9 find the body and its variable names through
the literal alone, without knowing which table, if any, the body was
published into. The predicate entry is not enough on its own: a query's
group predicate is shared by every query with an aggregate at that
position and arity, section 5.2, and each such query has a group rule of
its own; an atom's predicate says nothing about which variables the
braces named.

---

## 7. Stratification

`specs/design-stratify.md` section 2 makes an edge per relational body
literal, positive or negative. An aggregate literal makes a third kind:

```text
p(X) :- q(X), not r(X), N = count { s(X, Y), t(Y) }, M = count { u(X, Z) }.

p -> q                    positive
p -/> r                   negative
p =/> agg$p$1$0$3           aggregate, to the group predicate
agg$p$1$0$3 -> s            positive, from the group rule
agg$p$1$0$3 -> t            positive, from the group rule
p =/> u                   aggregate, to the atom folded in place
```

The edge's target is the predicate the fold reads, section 6: the group
predicate of a body compiled to a group rule, and the atom's predicate of
a body folded in place, section 5.6, which the group rule's one positive
edge would have led to.

**An aggregate edge is a negative edge for every purpose of the analysis.**
A program is stratified when no negative and no aggregate edge has both
ends in one component; the stratum of a component is the largest over its
edges to other components of `stratum(target)` for a positive edge and
`stratum(target) + 1` for a negative or an aggregate one; and the schedule
is the components sorted by stratum, stably. So the relation a fold reads
is complete before any rule holding its aggregate runs, exactly as a
negated predicate is, and the fold reads a relation that will not change.
Section 7 of the stratification design, that a component's rules read only
complete relations or its own, holds with "reads" covering the fold. The
two forms of the edge place the reader alike: a strict edge to a group
predicate whose one positive edge leads to `u` puts the reader at
`stratum(u) + 1`, and a strict edge to `u` puts it there too.

The group predicate itself is derived by one rule and read positively by
nothing, so in a stratified program it is a component of its own, never
recursive, evaluated in one pass, in the stratum its body's edges give it.
Its rule reads what the body reads, so a body over a recursive predicate
waits for that predicate's fixed point, and a body over facts alone reads
base stores in stratum zero. In a program that is not stratified a group
predicate may lie on the offending cycle, and the diagnostic below is
worded so that the writer never has to know it exists.

**The diagnostic names the aggregate.** The unstratified error and its
notes are those of `specs/design-stratify.md` section 5, with the edges of
a group rule attributed to the aggregate they came from. The **closing
edge** is the first negative or aggregate edge whose ends share a
component, in rule and literal order **over the outer rules**: a group
rule's own negative edge is never chosen, since every cycle through a
group predicate passes through the aggregate edge that reaches it, so an
offending component always holds a closing edge of an outer rule, and the
error points at that rule as it points at the rule holding a negated
literal today. The cycle is the shortest one through the closing edge,
found by the same search, and it is a sequence that begins at the closing
edge's target and ends at its source, which is an outer rule's head. In
that sequence **an aggregate edge to a group predicate is followed by an
edge of its group rule**: only an edge of the group's one rule leads out
of a group predicate, and the sequence cannot end there. Each such pair is
reported as one note, in the program's vocabulary, keeping the group
edge's polarity and pointing at the literal inside the braces that
continues the cycle -- `depends on` for a positive literal there and
`depends negatively on` for a negated one. **An aggregate edge to an atom
folded in place is one note by itself**, `<source> depends on <target>
through an aggregate`, pointing at that atom, which is the one literal of
the aggregate's compiled body, section 6, and not at the aggregate literal
of the outer rule where the edge was made; it reads as the pair would
have, the pair's second edge being the copy of that atom, and for a
self-cycle, section 4.7, it is the whole report under the error, the path
beyond the closing edge being empty. The analysis knows a vertex is a group predicate by
the predicate's kind, which the compiler marked when it named it, section
5.2, and not by its being an aggregate edge's target, which an ordinary
predicate now can be:

```text
paths.dl:9:1: error: program is not stratified
paths.dl:9:47: note: shortest/3 depends on shortest/3 through an aggregate
```

for section 4.7's rule, and, for a longer cycle,

```text
paths.dl:9:1: error: program is not stratified
paths.dl:9:36: note: shortest/3 depends on dist/3 through an aggregate
paths.dl:12:5: note: dist/3 depends on shortest/3
```

A generated name appears in no note: the group predicate is how the
analysis sees the aggregate, not how the writer does, and the writer fixes
the cycle at the literal the note points to.

The dependency analysis runs over generated rules too, as a guard,
`specs/design-magic.md` section 9.1; an aggregate edge inside a rewrite's
component would be an internal error there for the same reason a negative
one is, and the rewrite never makes one, section 11.

---

## 8. Planning

### 8.1 Placement

`specs/design-plan.md` section 5 turns each literal into an operation at
the point it is placed, walking a bound set, and `specs/design-arithmetic.md`
section 7.1 adds the assignment. One rule is added beside it:

* an **aggregate literal** whose shared slots are all in the bound set
  becomes `AGGREGATE`, and adds its result slot to the bound set. When
  its result slot is already bound -- by a generator, an `ASSIGN` or an
  `AGGREGATE` placed before it -- it becomes `AGGREGATE` as well, and
  compares rather than binds, section 9.1, as an equality over two bound
  sides is a filter. The slot's kind was fixed by the operation that bound
  it, `specs/design-arithmetic.md` section 7.2, and the comparison reads
  the slot by that kind.

It is placed with the tests, at the earliest point every shared slot is
bound, in written order among the tests placeable at the same point, and
placing it can make further tests and assignments placeable, so the
placement repeats until nothing more is, before the next positive literal
is scored. `N = count { ... }, N > 3` places the fold as soon as its key
is bound and the filter right after it. The greedy order of
`specs/design-planner.md` section 5.1 is untouched: positive literals are
scored against the bound set as it stands, which may hold a result slot,
and a literal keyed on one has a bound argument and is scored and accessed
accordingly. The one-pass ranking of `specs/design-planning-cost.md`
section 3 holds, since an aggregate placed is a mask change like any
other.

Earliest is the same choice `specs/design-plan.md` section 5.1 makes for
filters, and it is not always cheapest: a fold whose result is read only
by the head is work done before the literals after it have rejected the
bindings that never reach the head. A cost model that placed a fold late
when nothing filters on its result would need an estimate of the group's
size that the planner does not have, and section 20 leaves it until a
workload asks. What the placement never affects is the answer, section
4.6.

### 8.2 The operation

```c
enum relic_op_kind {
	RELIC_OP_SCAN,
	RELIC_OP_LOOKUP,
	RELIC_OP_MEMBER,
	RELIC_OP_ANTI_MEMBER,
	RELIC_OP_FILTER,
	RELIC_OP_ASSIGN,
	RELIC_OP_AGGREGATE,	/* fold a group relation's rows for a bound key */
	RELIC_OP_EMIT
};

struct relic_op_aggregate {
	relic_relation_t	*relation;	/* the source relation, complete */
	const uint16_t		*key_slots;	/* the key columns' slots, in column order */
	relic_location_t	 where;
	uint32_t		 key_mask;	/* the key columns, the literal's */
	uint32_t		 index_position;	/* when the access is a lookup */
	uint16_t		 key_count;
	uint16_t		 column;	/* the aggregated column */
	uint16_t		 slot;		/* the result */
	enum relic_aggregate_fn	 fn;
	bool			 binds;		/* false: the slot was bound before; compare */
};
```

| Operation | Reads | Binds | Succeeds |
| --- | --- | --- | --- |
| `AGGREGATE` | the key slots, the group relation's tuples for that key | its result slot, or nothing when `binds` is false | once or never |

`AGGREGATE` is a test that binds, like `ASSIGN`: it reads bound values,
produces at most one match, and leaves one new binding in the frame. It
never runs out of matches as a generator does; it fails when its function
has no value over the group (section 4.4, or an empty group under `min`
and `max`), or when it compares a bound result slot against a different
value, and then the binding is rejected as a filter rejects one. The three
invariants of `specs/design-plan.md` section 6 hold, with the second read
as "some earlier operation binds, a generator, an `ASSIGN` or an
`AGGREGATE`".

### 8.3 Access and slot kinds

The group relation is complete, so the operation reads it through `all`,
`specs/design-evaluation-cost.md` section 3.1, and never through a
member's view: an aggregate literal never reads a member of its own
component, section 7, so it has no delta variant of its own and, in a
delta variant of another literal, folds the same complete relation every
round, as `ANTI_MEMBER` tests the same one.

The rows the fold visits are the tuples whose key columns, the literal's
`key_mask`, hold the key, and the planner chooses how they are reached by
the rule of `specs/design-planner.md` section 5.2, as it does for a
positive literal with the same bound columns:

* with **no key column** the fold walks the whole relation, since the
  group is then the whole of it;
* with **some key columns** it walks **the index keyed on exactly the key
  columns**, built by the relation on first use as one is for a `LOOKUP`,
  and kept with the relation's other indexes, so a program's or a query's
  fold over the same relation on the same columns finds it built; under
  `RELIC_F_NO_INDEX` it walks the whole relation and matches the key
  columns of each tuple, as a generator with bound columns is scanned
  under it, so the reference strategy has no index the optimized one
  lacks;
* with **every column a key column** the group is the tuple the key names
  or nothing, and the walk is one probe of the canonical full-tuple index,
  as a `MEMBER` is.

For a group relation the key columns are its leading ones; for an atom
folded in place they are whichever columns hold the shared variables,
section 5.6, which is the position a `LOOKUP` with the same bound
arguments is in, and an index is keyed on a set of columns, not on a
prefix.

`index_position` names the index in the second case and none in the
others, and which case applies is written into the operation, so the
executor has no branch on the strategy.

The result slot's kind, `specs/design-arithmetic.md` section 7.2, follows
from the function: **`count` and `sum` bind a number slot**, their result
being an integer the executor computed, and **`min` and `max` bind a
domain slot**, their result being a value the group holds, which has an
identifier already. The planner writes the kind into every later read of
the slot, as it does for an assignment's, and a positive literal keyed on
a result slot is looked up through the slot's frame entry by the rule of
section 8.2 there.

---

## 9. Execution

### 9.1 The fold

An `AGGREGATE` builds its key from the key slots' frame entries, laid out
as a tuple of the relation with the key columns filled and the others
unread, which is how an index is probed, walks the chain the index holds
for it -- or the relation's range when there is no key, matching each
tuple's key columns -- and folds each tuple's aggregated column into an
accumulator in the executor scratch:

```text
count:      rows = rows + 1
sum:        the column decodes to an integer, or the fold has no value;
            add it exactly, section 9.2
min, max:   the first row fixes the type and the extreme;
            a later row of another type ends the fold with no value;
            otherwise compare the decoded values and keep the extreme's
            identifier
```

At the end: `count` binds the row count; `sum` checks that the total fits
and binds it, or records the error of section 4.5; `min` and `max` bind
the extreme's identifier when a row was seen, and otherwise fail with no
note, an empty group being the ordinary meaning of section 4.3 and not a
mistake. A fold that met a value outside its function's domain fails and
records the note of section 4.4. When `binds` is false the fold's value is
compared with the slot's binding instead, and the operation holds or
fails: the value is a cell of `specs/design-arithmetic.md` section 8.1, an
integer for `count` and `sum` and an identifier for `min` and `max`, the
slot's binding is the cell its kind gives, an integer from `numbers` for a
number slot and an identifier for a domain slot, and the two are compared
for equality by the rule of section 4.3 there, which covers every pairing.
A count of two against a number slot holding two is equal whether or not
the table holds the integer; the frame entry, which may be zero, is not
what is compared.

A number slot's integer goes into `numbers[slot]` and its frame entry is
set by the one read-only probe of `specs/design-arithmetic.md` section 8.2,
`relic_value_find_integer()`, so that every later read of the slot -- a
key built from it, an expression over it, a head column emitted from it
-- needs no case of its own. A count that the table lacks is a pending
integer in the head tuple and is interned when the batch is stored,
section 8.3 there; a head column from a `min` or `max` is an ordinary
identifier.

The walk holds no tuple pointer across the operations below it, per
`specs/design-eval.md` section 2.2: the fold completes before the
operation reports its match, so nothing below it runs while the chain is
being walked, and the relation is complete, so nothing inserts into it in
any case. The chain is walked in identifier order, which is insertion
order, and section 4.5 makes the answer independent of that order.

The cost is one tuple read and one decode per row of the group, per
binding of the outer rule that reaches the operation. A rule that binds
the same key many times folds the same group many times; section 20 notes
a relation of folded results as the optimization for that shape.

### 9.2 The sum accumulator

`sum` accumulates in **128-bit two's complement, in two 64-bit words**,
without `__int128`, which `configure` would have to test for and some
targets lack:

```c
struct relic_sum {
	uint64_t	lo;
	uint64_t	hi;	/* two's complement high word */
};

void	relic_sum_add(relic_sum_t *, int64_t);	/* lo + v with carry; hi + sign(v) + carry */
bool	relic_sum_fits(const relic_sum_t *);	/* hi is the sign extension of lo */
int64_t	relic_sum_value(const relic_sum_t *);	/* when it fits */
```

A tuple identifier is 32 bits, `specs/design-relations.md` section 4, so
a relation holds fewer than 2^32 tuples, and each addend is within 2^63 in
magnitude, so the total is within 2^95 and the accumulator never wraps; a
unit test drives it to that bound with each extreme, in each order, and
reads the total back. `relic_sum_fits()` is the one check, made once, and
there is no signed overflow anywhere in the fold for UBSan to find.

### 9.3 Notes and errors cross the executor's boundary

The note and the error are the two records of `specs/design-arithmetic.md`
section 8.4, with their marks and their messages. An overflowing sum ends
the plan as though every generator were exhausted and is reported by the
caller when it takes the batch, which discards the batch and fails the
evaluation; in a parallel round the task abandons its job and the round is
run again serially, so the message is the serial evaluator's. The note is
recorded once per rule through the origin every compiled rule carries,
`specs/design-eval.md` section 2.3; a group rule's origin is the statement
its aggregate was written in, so a mixed-type comparison inside the braces
counts against that rule.

### 9.4 The frozen round

`specs/design-parallel.md` section 3 runs a round's plans over a store
nothing writes. `AGGREGATE` reads a complete relation and writes its frame
and its accumulator, both in the scratch that is the task's own, and
interns nothing; it is compiled against `lib/frozen.h` like every
operation, and it needs nothing the arithmetic design did not already give
the round: a number slot in the frame, a pending integer in a candidate,
the measure and rewrite before the merge's first decline point, the error
that abandons a job. The identities of `specs/design-parallel.md` section
12 hold at every thread count for a program with aggregates, and the tests
of section 16 assert them.

A group rule is a rule of a component evaluated in one pass, which
`specs/design-eval.md` section 4 lets defer its insertions to the pass's
end, so its pass is shared among the pool's threads as any such pass is,
with no case for it.

### 9.5 Streamed rows

**A query whose plan holds an `AGGREGATE` is gathered**, into its answer
relation, and delivered when the plan has run to its end. `sum` can fail on
any binding, and a plan that can fail never streams,
`specs/design-arithmetic.md` section 9.3; `count` binds a number slot,
which may reach the head as a pending integer, and a streamed batch is
never resolved. `min` and `max` could stream, and do not, because a rule
that streamed one function and gathered three would be a list, and a
query with an aggregate in it is rarely large. The condition of
`specs/design-comparison-cost.md` section 4.1 gains the clause, and its
other clauses stand.

---

## 10. Queries

A query may hold aggregates; `node(X), N = count { edge(X, Y) }?` has
output columns `X` and `N`, the named variables of the outer rule in first-
occurrence order, since a local variable takes no slot of it. It is checked
by the closure of section 5.3, as a query is checked by the rule's
computation, so an unsafe shared variable is reported worded for a query.

**A query's group rules belong to the query**, not to the program's rule
table. Appending them to the program would make every prepared query with
an aggregate a change to the program, discarding what was derived and
forcing a materialization the query was prepared to avoid. So they are
compiled with the query's own rule -- into the program arena for a query
written in the program, into the query's arena for one prepared from text,
`specs/design-plan.md` section 7.2 -- and held by the query object, and
answering the query is one sequence whichever way the query is evaluated,
refining step 1 of `specs/design-eval.md` section 7:

1. **discard** what a stale or rewritten evaluation discards, as today;
2. **evaluate the program's part**: the whole program's fixed point when
   the query is not rewritten, or the complete part of
   `specs/design-magic.md` section 3 when it is, from the program's
   schedule;
3. **run each of the query's group rules once**, in body order, each into
   its group relation, a derived relation of a generated predicate as a
   rewrite's are; an aggregate folded in place, section 5.6, has none to
   run, its relation being one step 2 made complete;
4. **evaluate the rewrite's generated rules**, when the query is
   rewritten, from their own schedule;
5. run the query's plan once, or stream it;
6. stream the answer relation.

Step 3 is one pass per group rule, since a group rule is never recursive,
and everything a group rule reads is complete after step 2: its body's
predicates are read completely by the rule of section 11 when the query is
rewritten, and by the program's fixed point when it is not. **Step 3 comes
before step 4 because the rewrite may copy the aggregate literal.** The
rewrite appends the query's literals to the prefixes of the magic and
supplementary rules it drafts, section 11, so `N = count { v(X) }, p(N)?`
drafts `magic$p$b(N) :- N = count { v(X) }.`, a generated rule that folds
the query's group; a group filled after the generated rules ran would have
been read empty by that seed, and the query would demand `p(0)` and find
nothing where the whole program answers. A rewrite abandoned over its
budget falls back to the whole program's fixed point in step 2 and has no
step 4, and the sequence stands.

**The group relations are emptied when the run ends**, with the answer
relation, so outside a run a query holds nothing derived, and running it
again derives the same rows in the same order. What a run may leave behind
is an index: a fold in place over a program relation or a base store
builds the index on its key columns in that relation, section 8.3, which
stays with the relation and is counted against the memory limit as the
index a query's `LOOKUP` builds is, until the relation is discarded. A run
that fails in step 3 fails as an evaluation fails, before any row.

Under `-a`, or after `relic_materialize()`, the program's own group rules
have been evaluated with the program and the query's are evaluated by the
query; the relations are current, per `specs/design-eval.md` section 6,
because a query's group rules count in no pair and their relations are
empty when no query is running.

---

## 11. Magic Sets

The rewrite of `specs/design-magic.md` treats an aggregate literal as it
treats a negated one, and for the same reason: a fold needs the whole
group, and architecture section 37's rule that a relation read whole is
materialized in its lower stratum keeps the rewrite simple.

**The relation a fold reads is read completely**, and so is everything the
body reads. The walk of section 3 there gains one case: an aggregate
literal adds the predicate it folds, section 6, to the roots and visits
its compiled body, adding every predicate a positive or a negated literal
of it names to the roots. For a body compiled to a group rule that is the
group predicate and what the group rule reads; for an atom folded in place
the two are the same predicate, named once by the literal and once by its
one-literal body, and the walk needs no case to tell them apart. The
complete part is closed over components as before, so a
program rule's group rule and the components its body depends on are
evaluated in phase 1 from the program's schedule; a query's group rule is
run by the query, after phase 1 has made what it reads complete and
**before any generated rule runs**, step 3 of section 10, since a
generated rule may hold a copy of the literal that reads it. A predicate
the query reaches both through an aggregate and through a positive
literal is read completely everywhere, as one both negated and called
positively is.

**The literal is appended as it is.** In the order `relic_plan_order()`
gives a body, the aggregate literal is a test placed where its shared
slots are bound, and the order function reports that it binds its result
slot, as it reports an assignment's binding, `specs/design-arithmetic.md`
section 10; a call after it whose argument is the result is adorned bound.
The rewrite copies the literal into prefixes with its shared and result
slots renumbered and the predicate it folds unchanged, never adorns one,
and hashes and compares the literal structurally -- function, folded
predicate, key columns, aggregated column, slots -- when generated rules
are deduplicated. The check that a negated literal in a generated rule
reads a complete predicate covers an aggregate literal too: one over a
predicate not read completely would fold an empty relation, or a demanded
part of one, and fails the query with an internal error instead.

**A literal's size is one.** The literal holds no expression tree and its
body is not copied, since what it folds is read completely and never
rewritten, so the budgets of `specs/design-magic.md` section 8 charge it
one literal wherever they charge a literal.

The equivalence `magic(Q, P) == Q(full_materialize(P))` holds by the
argument of section 9.3 there: an aggregate literal reads, in the
rewritten program, the relation full materialization computes, and holds
for exactly the bindings it holds for there.

---

## 12. Explain

A rule is shown as written, its aggregate literals as `N = count { ... }`,
`S = sum(Size) { ... }`, `M = min(D) { ... }` and `M = max(D) { ... }`,
with the body printed from the literal's compiled body under the variable
names it carries; `_` is written `_` and constants as the text output
format writes them. A group rule is shown as the generated rule it is,
under its own component, as a rewrite's generated rules are; an atom
folded in place has no rule to show. The operation prints as its kind,
the equality it stands for, the relation it folds with its columns named
from the compiled body, and its key columns:

```text
aggregate S = sum(Size) agg$dir_size$2$0$2(Dir, F, Size) key={0}
aggregate N = count agg$leaves$1$0$1(X)
aggregate M = min(D) dist(X, Y, D) key={0,1}
aggregate N = count depends(T, D) key={0}
aggregate N = count edge(Y, X) key={1}
aggregate N = count valueAlias(A, B)
```

The first two fold group relations, of a body of two atoms and of a body
with a negated literal; the rest fold the atom's own relation, section 5.6:
the third, section 2's `shortest/3`, with every column a key column, the
fifth keyed on its second column because that is where the shared variable
stands, and the last the whole relation, having no key. For section 2's
`dir_size/2` under `-a`:

```text
stratum 0, component 0: agg$dir_size$2$0$2/3
    rule agg$dir_size$2$0$2(Dir, F, Size) :- file(F, Dir), size(F, Size).
        scan file(F, Dir)
        lookup size(F, Size) key={0}
        emit agg$dir_size$2$0$2(Dir, F, Size)
stratum 1, component 1: dir_size/2
    rule dir_size(Dir, S) :- dir(Dir), S = sum(Size) { file(F, Dir), size(F, Size) }.
        scan dir(Dir)
        aggregate S = sum(Size) agg$dir_size$2$0$2(Dir, F, Size) key={0}
        emit dir_size(Dir, S)
```

A query's group rules are shown under the query's heading, before its
plan, each as a rule with its plan; a query whose aggregates are all folded
in place shows only its own:

```text
query $q0(X, N) :- node(X), N = count { edge(X, Y), not blocked(Y) }.
    rule agg$$q0$2(X, Y) :- edge(X, Y), not blocked(Y).
        scan edge(X, Y)
        anti-member blocked(Y)
        emit agg$$q0$2(X, Y)
    scan node(X)
    aggregate N = count agg$$q0$2(X, Y) key={0}
    emit $q0(X, N)
query $q1(X, N) :- node(X), N = count { edge(X, Y) }.
    scan node(X)
    aggregate N = count edge(X, Y) key={0}
    emit $q1(X, N)
```

Every line is printable ASCII, per `specs/design-planner.md` section 8.3:
a function name, a variable name and a generated name need no escaping,
and a string constant is escaped as it is today.

---

## 13. Diagnostics

All follow architecture section 56: one line, lowercase, located at the
byte to look at.

Parse errors, one per statement:

```text
rules.dl:3:14: error: an aggregate is written V = count { ... }
rules.dl:5:9: error: count takes no variable
rules.dl:6:9: error: sum takes one variable in parentheses
rules.dl:7:13: error: expected a variable, found an integer literal
rules.dl:8:30: error: an aggregate body cannot hold an aggregate
rules.dl:9:17: error: expected a literal, found '}'
```

Check errors, every offender of a rule reported:

```text
rules.dl:3:56: error: variable T in the aggregate of rule above/1 is not bound by a positive literal or an assignment of the aggregate body
rules.dl:3:56: note: a variable bound outside an aggregate must also be bound inside it
rules.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal or an assignment
rules.dl:12:207: error: the aggregate in rule wide/1 mentions more than 32 variables
rules.dl:9:1: error: program is not stratified
rules.dl:9:36: note: shortest/3 depends on dist/3 through an aggregate
rules.dl:12:5: note: dist/3 depends on shortest/3
```

Evaluation errors, the first met in serial order, which fail the
evaluation:

```text
rules.dl:9:14: error: integer overflow in rule dir_size/2
<query>:1:20: error: integer overflow in the query
```

The note of verbosity level 2, once per rule:

```text
rules.dl:4:22: note: a row of sum(Size) holds a value that is not an integer; the aggregate has no value
rules.dl:7:19: note: the rows of min(N) hold values of more than one type; the aggregate has no value
```

No new warning. A result variable the rule never reads is a singleton and
gets the singleton warning; no singleton is reported from inside a group
body, section 5.4.

---

## 14. Limits and untrusted programs

| Limit | Value | Where enforced |
| --- | --- | --- |
| variables of a group body, shared and local together | 32 | `lib/rule.c`, at the occurrence that would exceed it |
| nesting of aggregates | none | `lib/parse.c`, at the inner function name |
| a group relation's tuples and storage | `max_tuples`, `max_memory`, with everything else | `lib/relation.c`, before an insertion or a growth |

The arity bound is D-7's, since a group predicate is a predicate. The
nesting bound is structural for the reason the expression depth is: it
bounds the parser's recursion, and a program the parser accepts can then
be compiled and printed without a stack check.

A program relic reads may have been written by someone who wants it to
misbehave, and aggregates give such a writer three new tools, each of
which this design takes away:

* **undefined behaviour.** The sum is exact in 128 bits and checked once,
  section 9.2, so no signed overflow is ever executed; `count` cannot
  overflow; the parser's descent through a group body is one level deep,
  section 3.4, so no text overflows the C stack.
* **unbounded memory.** A group relation is a relation and is charged to
  the tuple and memory limits before it grows; the message names it as the
  reader wrote it, as an aggregate of the predicate whose rule holds it --
  `limit exceeded: tuples=20 while deriving an aggregate of pairs/1` -- or
  as an aggregate of the query, since its generated name says nothing the
  reader can change and `-e` shows it. An atom folded in place, section
  5.6, stores nothing but the index on its key columns, charged to the
  memory limit as every index is, refused with the message a `LOOKUP`'s
  index is refused with, which names the relation indexed, and kept with
  the relation as a `LOOKUP`'s is. A fold allocates nothing, its
  accumulator being a fixed part of the scratch.
* **unbounded time.** A fold is linear in its group, and a group is a
  relation the limits bound. What the limits do not bound is the work of
  deriving a group: `count { b(_), b(_), b(_) }` joins `b` with itself
  three times and collapses every combination into one row of no
  columns, so the relation stays at one tuple while the work grows as a
  power of the body's length. That is the position every join is in
  today, a body of `k` wildcard literals over one relation costing its
  size to the `k` whether or not an aggregate reads it: the limits bound
  what is stored, not what is computed, and no step or time limit exists,
  `specs/design-api.md` section 11, which `TODO.md` records for 1.0. This
  design adds no new way to spend time that a rule without an aggregate
  did not have.

Nothing a fold computes reaches a caller unformatted: a count or a sum is
an `int64_t` and prints as one, and a `min` or `max` is a value the group
held. No diagnostic quotes a value from a group, so none can carry text a
program chose; the generated names a limit message quotes are built from
positions and a predicate name the program wrote, shortened as every name
in a message is.

---

## 15. Interface

**The library's interface gains one flag and no other change.**
`RELIC_F_GROUP_RULES` joins the strategy flags of `include/relic.h`,
documented beside `RELIC_F_NO_INDEX` and the others as what it is: a
switch that compiles every aggregate body to a group rule, for comparing
the two forms, which no program needs. No function, type or structure
field is added: values are still symbols, strings and integers, and an
aggregate's result in a row is one of them. What else changes is text:

* `relic_check()`'s description of range restriction gains the aggregate
  literal, the shared-variable rule of section 5.3, and recursion through
  an aggregate beside recursion through negation;
* `relic_query_run()` says that a query whose plan holds an aggregate is
  gathered, and that an overflowing sum fails a run as an undefined
  arithmetic result does;
* `RELIC_F_NOTE_MIXED`'s description names the aggregate notes;
* `relic_stats_t`'s `derivations` and `tuples_inserted` are described as
  counting group relations, which a program with an aggregate over a body
  of more than one atom derives; a body that is one atom of distinct
  variables derives nothing, section 5.6.
* `-X group-rules` in the tool is `RELIC_F_GROUP_RULES`, listed in
  `relic(1)` with the other `-X` behaviours, which are not part of the
  tool's stable interface.

The library's interface version is not bumped, since the binary interface
is unchanged.

**The tool does not change.** An overflowing sum is an ordinary error with
a nonzero exit, `-vv` reports the aggregate notes with the others, `-l
tuples=` and `-l memory=` bound group relations, and `-e` prints the
group rules and the operation. The manual pages describe the four
functions, the set reading of a group with the `edge(a, _)` example of
section 4.1, the shared-variable rule and its idiom, what an aggregate with
no value does, and that an aggregate may not read a relation that depends
on it.

---

## 16. Tests

Every test is added with the change that needs it and is run against the
tree before it, per `CLAUDE.md`. Unit tests are in the C harness of
`tests/unit/`; language tests are `.dl` files with their expectations.

**Lexer.** The two brace tokens; `{` inside a string still data.

**Parser.** Each function with and without a variable, against the tree it
must build; a body of one literal and of several, with negation, a
comparison and an assignment inside; `_` inside a body; `count`, `sum`,
`min` and `max` as predicate names and as symbol constants, including `X
= count` followed by `,` and by `.`; every parse error of section 13 at
its byte, one per statement; every v1 program of `tests/lang/` producing
the tree it produced.

**Check.** Shared and local classification: a variable shared through the
head, through a later literal, through an earlier one, through a second
body, and through the result position; the aggregated variable local; the
group rule's shape and name, for a program rule and for a query, and the
reuse of a query's name at the same arity; the outer closure firing an
aggregate after an assignment and an assignment after an aggregate; the
shared variable unbound inside the braces, with its note; the result
variable inside its own braces; a shared variable bound nowhere; 32 and
33 variables of one body; no singleton from inside a body and one for an
unread result; nothing appended from a statement whose outer rule is
unsafe; a load that fails at the reservation leaving the table unchanged.

**Stratification.** Aggregate edges in the graph and in the stratum
computation; a group predicate as a singleton component; the collapsed
note for a self-cycle and for a longer one; an aggregate over a negated
predicate's stratum; an aggregate edge and a negative edge in one
component reported once.

**Plan and executor.** The operation placed at the earliest point its key
is bound, before and after its written position; a compare when the
result slot is bound already; the index keyed on the shared columns, and
a whole-relation walk with none; each function over a group of one row,
of many, and empty; `sum` over a symbol and `min` over two types failing
with the note; `sum` exactness: `INT64_MAX, 1, -1` and `1, -1, INT64_MAX`
both succeeding, `INT64_MAX, 1` failing, and the accumulator unit-tested
at 2^31 addends of each extreme in every order; the error record
crossing the executor's boundary and failing the evaluation with nothing
inserted from the batch; a count reaching a head as a pending integer and
as an interned one; a `min` result keyed into a later lookup; a query's
group relations filled in step 3, before any generated rule runs, and
empty after the run; a query with an
aggregate gathered.

**Folding in place.** Which bodies are relations: one atom of distinct
variables, nullary included, folded in place; a wildcard, a constant, a
repeated variable, a second literal, a comparison or a negation, each
compiled to a group rule. The literal's folded predicate, key columns and
aggregated column for a shared variable in the first, a middle and the
last column, for two shared variables out of order with their slots, and
for none; the compiled body headless with slot `c` as column `c`; no
group predicate interned and no rule appended for such a statement, and
the rule table and predicate table counts saying so. The planner's access
for a key that is not a prefix: the lookup's index keyed on exactly those
columns, found again by a second fold on the same columns, the scan under
`RELIC_F_NO_INDEX` matching those columns, and the probe when every
column is a key column. The executor's probe tuple with the key in the
right columns, against a relation whose rows differ only in a non-key
column. Stratification: an aggregate edge to an ordinary predicate placing
the reader one stratum above it, the same stratum the group rule gave; a
recursion through an in-place fold rejected with the one note pointing at
the atom, `shortest/3 depends on shortest/3 through an aggregate`; a
group vertex recognized by its predicate's kind when another aggregate
edge ends at an ordinary predicate of the same component. Magic: the atom
read completely when the query also calls it bound, a seed rule folding it
in place before any generated rule runs, and two literals folding one
predicate with different key columns kept distinct by the deduplication.
The same answers under `RELIC_F_GROUP_RULES`, with the group rule back in
`-e`. The exhaustion sweep of `tests/unit/test_exhaustion.c` keeps a body
of two atoms among its aggregates, so that the all-or-nothing append and
the query's group run stay under allocation failure, and gains the fold's
index build as a point where memory runs out; its whole-statement check
reads the literal's `source` and `body`, and expects a group rule only
when the body has a head. The random tester's derivation oracle runs under
`RELIC_F_GROUP_RULES`, section 5.6.

**Language tests**, each a golden file:

```text
aggregate-syntax        every function, a body of several literals, names reused as predicates
aggregate-count         dep_count/2 of section 2, with a task of no dependencies
aggregate-sum           dir_size/2 of section 2
aggregate-minmax        min and max over integers, symbols and strings; an empty group
aggregate-project       edge(a, Y) against edge(a, _): 2 against 1
aggregate-shared        a shared variable bound inside and outside; the threshold idiom
aggregate-unsafe        the shared variable unbound inside; the result inside its own body
aggregate-nested        an aggregate inside a body, rejected
aggregate-stratify      section 4.7's rule and a two-rule cycle, with their notes
aggregate-shortest      dist/3 bounded, then min over it
aggregate-mixed         a symbol under sum and two types under min, with their -vv notes
aggregate-overflow      the error, no rows, nonzero exit, in both orders of the rows
aggregate-query         aggregates in a loaded query and in one prepared from text
aggregate-magic         a bound query over a rule with an aggregate: the group read completely
aggregate-explain       -e over section 12's program and query
aggregate-parallel      dep_count over a large graph at one thread and with -X parallel
aggregate-limit         -l tuples= refused while deriving a group relation
aggregate-assign        arithmetic inside a body, and an aggregate feeding an assignment
aggregate-relation      bodies folded in place and bodies that are not, with -e showing which, and the key on a middle column
```

**Equivalences.** `tests/equiv/strategies.sh` runs the aggregate corpus
under every strategy -- naive, no-index, no-reorder, no-magic, parallel,
their combinations, and group-rules alone -- and compares what it compares today, in the
groups `specs/design-hardening.md` section 3.1 states: every strategy's
answers equal the default's as sorted sets, and the tuple total of a
strategy that evaluates the same rules as its reference equals the
reference's, with the aggregate notes filtered from the compared standard
error as the ordering note is. A rewritten query and the whole program
evaluate different rules and hold different relations, so nothing beyond
their answers is compared between them, and `integer_values` is an
identity only between the same evaluation at different thread counts, as
`specs/design-arithmetic.md` section 9.2 states it. `aggregate-overflow`
is pinned per strategy, as `arithmetic-strategy` is, and must fail under
every strategy whose evaluation reaches the overflowing group, since
section 4.5 makes the failure a function of the group rather than of the
order its rows arrived in; a query the rewrite answers without reaching
that group succeeds, which is the difference section 4.5 admits, and the
golden file records which. The identity at several threads is asserted
after a successful run, after a run a tuple limit fails and after a run an
overflowing sum fails.

**Generated programs.** The generator of `specs/design-hardening.md` gains
aggregate literals, stratified and safe by construction: a body drawn
from predicates at levels strictly below the head's, so the aggregate
edge falls with the negative ones and section 2.3's argument holds; the
body's variables drawn fresh, with a subset of the head rule's positive
variables shared and each shared one placed in a positive literal of the
body; the result read by the head or a later comparison; `sum` over
predicates whose integers are small enough that no group's total can
overflow. So every strategy is compared on every program and a
disagreement is a bug. The fuzz dictionary gains the braces and the four
names, and the loader and evaluator targets exercise the parse errors,
the check errors and the overflow error, which must be reported and never
crash.

**Sanitizers.** `make check-asan` over the rule compiler's two-pass build
and the query's group relations; `make check-ubsan` over the fold and the
accumulator; `make check-tsan` over the parallel identity tests.

---

## 17. Order of work

Each step lands with its tests and is measured against the one before it.

1. **Front end and program model** (sections 3, 5, 6, 7, 12's rule
   printer, 13's parse and check errors). The braces; the aggregate
   literal in the tree; shared and local classification and the two-pass
   build; the group rule, its name and the all-or-nothing append; the
   closure; the aggregate edge and the collapsed note; the printer. Every
   v1 test holds; the new parse, check, stratification and printer tests
   hold. No plan runs an aggregate yet: the planner refuses a rule with one
   with an internal diagnostic, so nothing is half-evaluated.
2. **Plan and executor, serial** (sections 8, 9.1 to 9.3, 9.5, 10, 12, 15).
   `AGGREGATE`, its placement and index, the fold and the accumulator, the
   two slot kinds, the note and the error, gathered queries, a query's
   group rules and their emptying. The language and equivalence tests hold
   at one thread and under every strategy but `magic` and `parallel`.
   Gate: `dep_count/2` over the graph of `tc-cyclic` within 1.5 times the
   time of the join `depends(T, D)` alone.
3. **Magic Sets** (section 11). The walk's new case, the literal in
   prefixes and in the deduplication hash, the guard. `aggregate-magic`
   holds and `magic(Q, P) == Q(full_materialize(P))` holds over the
   aggregate corpus.
4. **The frozen round** (section 9.4). Nothing to build unless the
   identity tests say otherwise; the gate is the identity at every thread
   count after success and after each kind of failure, and `dep_count/2`
   scaling as a one-pass component does.
5. **Hardening and documentation** (sections 14, 16). The generator, the
   fuzz dictionary and corpus, the manual pages, a degree-count workload
   in `bench/compare/` measured against Souffle, which has aggregates, and
   `bench/baseline.txt` recorded again.
6. **Folding in place** (section 5.6, and its traces through sections 6,
   7, 8.3, 11 and 12). First the literal's `source`, `key_mask` and
   compiled body in the program model, with every body still compiled to
   a group rule, so the planner, the executor, the stratifier, the walk
   and the printer are moved to the new fields while nothing observable
   changes; then the compiler's one decision and `RELIC_F_GROUP_RULES`,
   with the tests of section 16 and the golden files whose `-e` output
   loses a group rule. Gate: a query counting the three CSPA relations of
   Graspan's httpd, `N = count { valueAlias(A, B) }` and its two
   companions, peaks at the memory of the fixed point it is folded over,
   which answering `valueFlow` over the same fixed point measures, where
   the copies peaked at four times it; `dep_count/2` within the time of
   step 2's gate.

Steps 1 and 2 are the feature; 3 and 4 make it complete under every mode
the tool has; 5 is what `CLAUDE.md` asks of every feature; 6 removes a
cost the first five accepted.

---

## 18. Determinism

* Which variables are local is a function of the statement's text, and
  the group rule's columns, its name and its position in the rule table
  follow from the text and the load order. Whether a body is folded in
  place is a function of its text and of `RELIC_F_GROUP_RULES`, section
  5.6, and never of what the relations hold.
* A group relation's tuples are inserted in the order the strategy
  derives them, per D-4, and every function's value is independent of
  that order: `count`, `min` and `max` trivially, and `sum` because it is
  exact over the group, section 4.5. So the answer, and whether a sum
  fails, are functions of the group's set of rows and the same under every
  strategy and at every thread count.
* An aggregate literal is placed by the planner as a function of the
  source and the strategy, so where each fold runs is the same on every
  run.
* A count computed by a fold is interned in derivation order at its first
  stored occurrence, `specs/design-arithmetic.md` section 9.2, so
  `integer_values` is the same at every thread count.
* An overflow names the first overflowing sum in serial order, because it
  is always the serial evaluator that reports it.
* Which rule's note is issued first may depend on the strategy, as it does
  today; nothing else about the diagnostics does. The aggregate notes name
  no type, section 4.4, so the row a fold meets first, which depends on
  the order the group's rows were inserted in, decides nothing a reader
  can see.
* `-e` prints a group rule from its compiled form and the operation from
  the plan, so equal programs print equal.

---

## 19. Amendments this design makes

* `specs/architecture.md` section 3: aggregates stay a v1 non-goal and
  point here; section 7: a variable local to a group body belongs to the
  body, section 4.2; section 13: the grammar of section 3.2; section 14:
  the shared-variable rule and the result variable, section 5.3; section
  15: an aggregate is a function of a complete relation of a lower
  stratum; section 30: an aggregate edge points strictly downward as a
  negative edge does, section 7; section 37: a group predicate is read
  completely, section 11; section 63: the arity limit on a group body;
  section 69: aggregates point here.
* `specs/decisions.md`: decision D-27 records the set reading of a group, the
  generated group rule, stratification below the reader, the exact sum,
  and the spelling.
* `specs/design-parser.md` section 3.2: the brace tokens and the four
  names recognized by position; section 5: the aggregate literal in the
  tree; section 9: the nesting bound; section 11: recovery inside a
  statement stays absent, and a group body is recovered with the
  statement.
* `specs/design-program.md` section 3: the group predicate's marks;
  section 4: local variables take no slot of the outer rule, section 5.4;
  section 5: the closure's aggregate clause, section 5.3; section 6: the
  new messages; section 8: `relic_aggregate_t`, section 6 here; section
  8.1: the two-pass build and the all-or-nothing append, sections 5.4 and
  5.5.
* `specs/design-stratify.md` section 2: the aggregate edge; section 4:
  counted as a negative edge; section 5: the collapsed note; section 6: a
  group predicate's component.
* `specs/design-plan.md` section 3: `AGGREGATE`, section 8.2 here; section
  5: the placement rule of section 8.1; section 6: the second invariant's
  wording.
* `specs/design-planner.md` section 5.1: the fold placed with the tests;
  section 5.2: the index keyed on the shared columns; section 8.2: the
  `aggregate` line and the group rules under a query, section 12 here.
* `specs/design-planning-cost.md` section 3: an aggregate grows the mask.
* `specs/design-eval.md` section 2.1: the accumulator in the scratch;
  section 5: the fold reads a complete relation as `ANTI_MEMBER` does;
  section 7: the sequence of section 10 here, and a query with an
  aggregate is gathered; section 8: an overflowing sum as a reason an
  evaluation fails.
* `specs/design-stratify.md` section 5, again: the closing edge is chosen
  over the outer rules, section 7 here.
* `specs/design-arithmetic.md` section 4.4: the aggregate as a second form
  of assignment, section 4.6 here; section 5: the closure's second clause;
  section 7.2: `count` and `sum` bind a number slot, `min` and `max` a
  domain slot; section 8.4: the aggregate note and the overflow through
  the same records; section 9.3: a plan holding an aggregate is gathered;
  section 19: aggregates are no longer absent.
* `specs/design-evaluation-cost.md` section 3.2: an aggregate literal, like
  a negated one, never reads a member and has no view.
* `specs/design-query.md` section 2: the query owns its group rules and
  their relations are emptied with the answer, section 10 here.
* `specs/design-comparison-cost.md` section 4.1: a plan holding an
  aggregate is gathered.
* `specs/design-parallel.md` section 12: aggregates among the programs the
  identities are asserted over.
* `specs/design-magic.md` section 3: the walk's aggregate case; section 6:
  the literal appended as it is; section 7: the literal in the canonical
  form; section 8: size one; section 9.1: the guard covers it; section
  9.2: a query's group rules run between the complete part and the
  generated rules, section 10 here; section 13: demand through an
  aggregate is absent as demand through negation is.
* `specs/design-facts.md` section 3.2: a group predicate is derived and
  has no base store.
* `specs/design-hardening.md` sections 2.3 and 2.4: aggregate literals
  generated stratified and safe by construction, section 16 here.
* `specs/design-cli.md` section 9: the aggregate notes at level 2.
* `specs/design-api.md` sections 3, 5.1, 7 and 11: the text of section 15
  here.
* `docs/todo-language.md`: the aggregates item points here.
* Folding in place, section 5.6: `specs/architecture.md` sections 30 and
  37 say the aggregate edge and the complete read are of the relation the
  fold reads, a group predicate or the atom's own; `specs/decisions.md`
  D-27 says which bodies get a group rule; `specs/design-stratify.md`
  section 5: the single note for an aggregate edge to an ordinary
  predicate, and a group vertex known by its predicate's kind;
  `specs/design-hardening.md` section 3.1: the `group-rules` strategy,
  and the derivation oracle under it; `specs/design-magic.md` sections 3,
  6, 7, 8 and 13 and `specs/design-program.md` section 8: the predicate a
  literal folds and its compiled body in place of the group predicate and
  the group rule; `specs/design-planner.md` sections 5.2 and 8.2,
  `specs/design-plan.md` section 3, `specs/design-eval.md` section 5 and
  `specs/design-evaluation-cost.md` section 3.2: the fold reads the
  relation it folds, a group's or an atom's own.

---

## 20. What is deliberately absent

* **Counting the ways a body is satisfied.** Section 4.1: a multiplicity
  the language cannot otherwise observe. A writer who wants it names the
  variables that make rows distinct.
* **Recursive and monotone aggregates.** Section 4.7. `min` over a
  recursive relation is written as a bounded recursion and a fold above
  it. Admitting it in recursion needs a lattice-aware store the append-only
  invariant of `specs/design-relations.md` section 4 does not have, and a
  semantics with conditions that are not obvious on first reading.
* **`mean`.** An integer mean is `sum` over `count` with the truncation of
  `specs/design-arithmetic.md` section 4.1, written in two literals; a
  mean with a fraction is `avg` of `specs/design-floats.md` section 7, a
  float over integer and float rows alike.
* **Nested aggregates.** Section 3.4. An inner aggregate is a rule of its
  own, named, and the nesting bound stays one.
* **Aggregates in heads, in expressions and as comparison operands.**
  Section 3.3.
* **`count` with a variable**, meaning the number of distinct values of
  it. `count { p(X, Y) }` already counts distinct rows; the distinct values
  of one column are the rows of a body that mentions only it, `count {
  p(_, Y) }`.
* **An explicit grouping list.** Section 4.2: the 1990 form named the
  grouping variables, and every later engine infers them from the rule,
  which is what a reader expects.
* **Witnesses.** The row that attains a `min` or `max` is found by a
  second literal over the same relation: `best(X, M) :- node(X), M =
  max(S) { score(X, S) }` gives the score, and `score(P, M)` in a rule
  that reads `best` gives who attained it. Souffle's witness syntax is
  sugar for that literal.
* **A shared variable bound only outside the braces.** Section 5.3: the
  prefix construction of `specs/design-magic.md` section 6 would supply it,
  at the cost of a relation per aggregate or a prefix evaluated twice, for
  a rule shape the idiom expresses.
* **Folding a selection or a projection in place.** Section 5.6: an atom
  with a constant could be folded through an index keyed on the shared
  columns and the constant's, and an atom with a wildcard under `min` or
  `max` through the relation it projects, since the extreme survives the
  projection. Each is a second way to build a key or a per-function case,
  for shapes the degree counts and totals that motivated section 5.6 do
  not take. Reopened when a workload shows one of them copying a large
  relation.
* **A relation of folded results.** Section 9.1: a second generated rule
  folding each group once, read by the outer rule with a default for an
  empty group. It would make a fold cost one lookup per binding rather
  than one walk, for a recursive outer rule or one that binds a key many
  times, at the price of a second relation, a generator that walks groups,
  and a lookup with a default. Reopened when a workload shows the walks
  dominating.
* **Placing a fold late.** Section 8.1: the planner has no estimate of a
  group's size.
* **Demand through an aggregate.** Adorning a group predicate on its shared
  columns is sound where adorning a negated one is not, since a demanded
  key's group is complete under the rewrite's completeness; it is left
  with demand through negation, `specs/design-magic.md` section 13, until
  a query shows the whole group relation costing more than its demanded
  part.
* **Streaming a query with `min` or `max`.** Section 9.5.
* **String and set aggregates.** Concatenation and collecting a group into
  a value need a value type the language does not have.
* **Sorting, `top-k` and `first`.** Result order is unspecified, per
  architecture section 11, and a `k` largest is a `max`, a negation and a
  recursion the writer can spell.
