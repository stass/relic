# relic negation design

**Status.** Design document for the negated literal: what it reads, what
`_` means in it, how the planner places it and the executor tests it, and
what the differential suite checks about it. Forward-looking, like every
document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 14, 15 and 37;
`specs/design-program.md` sections 4 and 5 for the safety computation the
literal is checked by; `specs/design-plan.md` sections 3 and 4 and
`specs/design-planner.md` section 5 for the plan it becomes;
`specs/design-eval.md` section 5 for the guarantee it tests under;
`specs/design-index.md` for the index it probes; `specs/design-magic.md`
section 13 and `specs/design-stratify.md` for the two analyses it leaves
unchanged; `specs/design-hardening.md` for the suite; `specs/decisions.md`
D-7, D-11, D-22 and D-31.

---

## 1. What this document settles

* what `not p(t1, ..., tn)` means when some of its arguments are `_`, and
  why the meaning is the only one that fits the rest of the language;
* which occurrences of `_` safety still rejects, and why the invariant
  safety rests on is unchanged;
* the operation a negated literal becomes: an existence probe keyed on the
  columns it reads, in its three shapes;
* why stratification and the Magic Sets rewrite need no change;
* what explain shows, what the diagnostics say, and what the random suite
  checks.

---

## 2. The literal

A negated body literal is `not` before an atom. Each argument is a
constant, a named variable or `_`:

```prolog
idle(C) :- customer(C), not order(_, C, _).
```

**The literal holds exactly when no tuple of the relation agrees with it on
the columns that are not `_`.** A constant agrees with a column holding that
value, a named variable with a column holding the value it is bound to, and
`_` with any column. Each `_` is quantified inside the negation: the literal
above says "there is no order tuple whose second column is `C`", not "for
some order tuple, the second column is not `C`".

That is the same thing as negating a helper relation projected onto the
named columns:

```prolog
order_of(C) :- order(_, C, _).
idle(C) :- customer(C), not order_of(C).
```

The two programs derive the same `idle`, and section 9 makes that identity a
property the random suite checks. Without the first form a program writes
the helper rule by hand, once per use, which is what makes a relation of
many columns unusable under `not`.

Two consequences follow from the meaning, not from the implementation:

* `not p(_, _)` holds exactly when `p` is empty. There is nothing to agree
  on, so the literal asks whether any tuple exists.
* A named variable of a negated literal must still be bound by a positive
  literal or an assignment before the literal is tested. `not p(X, Y)` with
  `Y` bound nowhere is not read as `not p(X, _)`: a variable the author
  named and used once is far more often a misspelling than an existential,
  and the singleton warning already says so for positive literals. The
  language has one spelling for "any value here", and it is `_`.

The same meaning holds for a negated literal inside an aggregate's braces,
which is compiled by the same code as a rule body,
`specs/design-aggregates.md` section 5.

---

## 3. Safety

`specs/design-program.md` section 5 checks a rule by one set computation:
*R*, the slots read by the head, the negated literals and the comparisons,
must be a subset of *B*, the slots bound by positive literals closed under
assignment. Section 4.1 of the same document gives `_` in a positive
literal no slot at all, because a wildcard is read by nothing.

**`_` in a negated literal is a wildcard too, and takes no slot.** It is
read by nothing: the probe of section 4 does not consult that column. So it
is compiled to the same wildcard argument a positive literal's `_` becomes,
enters neither *R* nor *B*, and the computation never sees it. The named
variables of the literal join *R* as before. Nothing else in the check
changes, and the invariant section 4.1 states survives word for word:
**in a rule that passes the check, every slot is bound by some positive
relational body literal or by an assignment.** A wildcard has no slot, so
there is still no unbound slot for the evaluator to meet.

Where `_` is still rejected, the reason is that it would be read:

* **In the head.** A head column must hold a value for every derived tuple,
  and `_` names none. The existing message stands: "anonymous variable in
  rule p/N cannot be bound by a positive body literal".
* **In a comparison.** `_ < 1` reads a value to compare. Same message.
* **As an aggregated variable.** `sum(_)` folds a value. The parser's
  message stands: "sum takes a named variable, not _".

The compiler tells the three apart from a negated argument by the position
it compiles an argument in. The positions are: binding, for an argument of
a positive literal; required, for an argument of the head or an operand of
a comparison; and **probed**, for an argument of a negated literal. A
named variable is bindable in the first and required in the other two; `_`
is a wildcard in the first and the third and takes a slot, so as to be
reported, only in the second. That is the whole of the change to the
middle end, and it covers a rule body, a query and an aggregate's group
rule alike, because all three compile their literals through the one
function.

---

## 4. The plan: an existence probe

A negated literal is a test, `specs/design-plan.md` section 3: it binds
nothing and succeeds at most once. It is placed once every named variable
of it is bound, which is the same question as before asked of the named
columns only, and it becomes `ANTI_MEMBER`.

**The operation carries a key: the columns that are not `_`.** Its terms
are a constant or a bound slot for each key column and `ANY` for each
wildcard column, and its key mask has a bit per key column, as a `LOOKUP`'s
does. The executor asks one question, **does the relation hold, inside the
operation's view, a tuple that agrees with the key**, and inverts the
answer. The key's shape decides how the question is asked:

| Key | Shape | How it is answered |
| --- | --- | --- |
| every column | full | the canonical index: find the tuple, check its identifier is inside the view, as `MEMBER` does |
| some columns | partial | the secondary index keyed on exactly those columns: probe the key, read the first tuple of its chain, check it is inside the view |
| no column | empty | whether the view is empty |

The partial shape is right because of what `specs/design-index.md`
promises of a chain: its tuples are in increasing identifier order, so the
first one is the lowest, and if it lies past the view's end no tuple of the
key does. An `ANTI_MEMBER` always reads through the `all` view, whose range
is the relation as it was when the plan started, and the negated relation
is complete by then, section 5, so "inside the view" is "exists".

The index is the ordinary secondary index, obtained when the literal is
placed by the same call a `LOOKUP` makes, and built then if the relation
has none on that key. It is charged to the memory limit as every index is,
and a build the limit refuses fails the planning of the component as it
does for a lookup, `specs/design-planning-cost.md` section 7. The relation
is in a lower stratum than the rule, so the build runs over a relation
that will not change, once, and every later plan over the same key finds
it. The invariant of `specs/design-plan.md` section 4 widens by one clause:
`BIND` occurs only in a `SCAN` or a `LOOKUP`; `ANY` occurs in those and in
an `ANTI_MEMBER`; neither ever occurs in a key column.

**Without indexes.** Under `RELIC_F_NO_INDEX` a `LOOKUP` is planned as a
`SCAN`. The partial probe cannot become a different operation, since its
answer is a yes or a no, so the executor answers it by scanning the view
for the first tuple that matches the terms, with the matcher a `SCAN`
uses, which already treats `ANY` as matching anything, and stops at the
first match. A test has no cursor to resume from, so the scan charges
every tuple it reads to the call's quantum and finishes its walk before
the executor may pause, as an aggregate's fold does,
`specs/design-interrupt.md`: a call overruns its quantum by at most one
such scan, and the strategy exists to check the indexed answer, not to be
fast. The full and empty shapes need no index and are unchanged by the
flag.

**Counters.** A probe counts one tuple looked up, as a `LOOKUP`'s probe
does, and the full shape counts as a `MEMBER` does. The counts are a
function of the bindings met, so they stay identical across thread counts,
which `tests/langtest.sh` requires.

The planner's estimate treats the literal as it treats every test: it
costs its placement and selects nothing it can measure,
`specs/design-planner.md` section 4. A test that probes an index is no
cheaper to place than one that probes the canonical index, so the order
does not change for any program that was planned before.

---

## 5. Stratification and the rewrite

**Stratification is unchanged.** The dependency graph has an edge per
relational literal from the rule's head to the literal's predicate,
negative for a negated one, `specs/design-stratify.md` section 2, and
never looks at an argument. `not order(_, C, _)` is a negative dependency
on `order`, exactly as `not order(O, C, D)` is.

**The Magic Sets rewrite is unchanged**, for three reasons that
`specs/design-magic.md` already states. A negated literal is never
adorned: demand does not pass through negation, section 13, and the
rewrite marks the component of a negated predicate complete and evaluates
it in full. A negated literal is copied into a rewritten body as it is,
after the prefix that binds its variables, section 7, and the copy renames
the variables of an argument and carries a wildcard through untouched. And
sideways information passing treats a negated literal as an ordering
constraint that binds nothing, so an argument that was never a variable
changes no adornment. A wildcard under `not` therefore reaches the
rewritten rule in the same position, with the same key, and is planned
there by section 4.

---

## 6. Diagnostics

The messages for `_` in a head, in a comparison and as an aggregated
variable are those of `specs/design-program.md` section 6 and
`specs/design-aggregates.md` section 4, unchanged in text. The message for
a named variable of a negated literal that nothing binds is unchanged too:

```prolog
idle(C) :- customer(C), not order(O, C, _).
```

```text
rules.dl:3:35: error: variable O in rule idle/1 is not bound by a positive body literal or an assignment
```

No new message exists, since the change removes a rejection and adds none.

---

## 7. Explain

Explain prints an `ANTI_MEMBER` as `anti-member` and the atom, with `_` for
a wildcard column, `specs/design-planner.md` section 8.2. The atom's text
already shows the key, since the key is every column that is not `_`, and
the probe's shape follows from it, so the line gains nothing for the full
and empty shapes. The partial shape adds the key as a lookup does, which
names the index the plan took:

```text
anti-member order(_, C, _) key={1}
```

Under `RELIC_F_NO_INDEX` the same line prints without the key, as a
lookup prints as a scan.

---

## 8. Tests

**Language tests** pin the meaning, each once as a rule and once as a
query where both apply: `not p(X, _)`; `not p(_, _)` over an empty and a
filled relation; a literal mixing a constant, a bound variable and `_`;
`_` under `not` inside braces; a bound query through the Magic rewrite, so
the literal is copied into a rewritten rule; and `-e` over the partial
shape, pinning the key. The rejections that stay are pinned beside them:
`_` in a head, in a comparison, as an aggregated variable, and a named
variable under `not` that nothing binds. `tests/equiv/strategies.sh` runs
each of these under every strategy, which is where a missing no-index
fallback would show, and `langtest.sh` reruns them in parallel.

**Unit tests** check the compiler's three positions: a wildcard in a
negated argument takes no slot and enters neither set, and `_` in a head
still takes one. They check the planner's key for each shape, the index a
partial key probes and the `ANY` of each wildcard column, with and without
indexes, and that a probe that scans charges its reads to the call's
quantum, so that consecutive scans over a large relation overrun it by at
most one scan.

**The random suite** generates `_` in negated atoms, in rule bodies and
in groups, with the frequency it gives a positive atom's arguments, and
its safety model counts a wildcard under `not` as needing no binding; the
derivation count stays on for programs whose only wildcards are under
`not`, since such a wildcard multiplies nothing. Section 9 adds the
property that is specific to this literal.

---

## 9. The helper-relation property

Section 2 gives the literal's meaning as an identity with a program that
has a helper rule. The random suite checks that identity directly, which
tests the probe of section 4 against the ordinary positive-literal path
that has been exercised since the suite existed.

For a generated program, the **helper rendering** replaces each negated
literal that holds a wildcard with a negated literal over a helper
predicate of the columns it names, and adds one rule deriving each helper
from its predicate, with `_` in the other columns:

```prolog
idle(C) :- customer(C), not order(_, C, _).

idle(C) :- customer(C), not order_w5(C).
order_w5(B) :- order(_, B, _).
```

A helper is named for its predicate and its wildcard columns, bit c for
column c, so the literals that leave out the same columns of one predicate
share one, and the name, holding `_`, is one the generator never draws for
a predicate. A helper reads its predicate positively and is read only under
`not`, where its predicate was, so its level is its predicate's and the
generator's stratification argument, `specs/design-hardening.md` section
2.3, holds for the rendering unchanged. The rendering is text the harness
writes rather than a program of the model, so the helpers count against
none of the model's sizes.

The harness loads the helper rendering under the default strategy and
compares each query's answer with the original's, as it compares strategy
against strategy, `specs/design-hardening.md` section 3.2. Tuple totals are
not compared: the helper relations hold tuples the original has nowhere. A
disagreement is a failure kind of its own, **helper**, in that document's
section 3.4, charged to no strategy and shrunk like any other, and the
self-test of its section 4.3 drops a row from the rendering's answers to
show that the comparison can fail.

---

## 10. Determinism

Nothing here depends on a pointer, a seed or a thread. The index a probe
takes is named by position, which is fixed until the relations are
discarded, as a lookup's is. Whether a probe finds a tuple is a function
of the relation's contents and the key, and the relation is complete, so
the answer is the same under every strategy; that is what sections 8 and 9
check.

---

## 11. What is deliberately absent

* **A free named variable under `not` read as existential.** Section 2
  gives the reason: `_` is the one spelling, and a named singleton is
  usually a mistake.
* **`_` in a head.** A head column must hold a value.
* **A second operation kind for the partial probe.** The three shapes are
  one question asked of three keys, and one operation whose key says which
  keeps the executor's dispatch and explain's text in step with the
  positive literal's `MEMBER`, `LOOKUP` and `SCAN`.
* **Demand through negation.** The rewrite keeps evaluating a negated
  predicate in full, `specs/design-magic.md` section 13; a probe keyed on
  fewer columns does not change what the negated relation must hold.
* **An index built ahead of the plan.** The index is built when the first
  plan asks, as every secondary index is, and shares the memory limit.
