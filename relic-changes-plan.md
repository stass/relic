# Plan for the language changes in relic-changes.md

Written against the tree at commit 2a8c38f (2026-10-03). Release 1.0.0 is
unreleased and untagged, so every "before 1.0" item is still open and a new
entry point, flag bit or configuration field still costs nothing.

The brief's section and file pointers were checked against the tree. Where
the tree disagrees with the brief, section 1 says so; those points need an
answer before the step that depends on them. Everything else is ready to
build in the order of section 2.

Three design documents accompany this plan, one per area where the design
carries real risk. Each takes the recommended answer to the question it
depends on and says so in section 3 below; a different answer changes a
section of the document, not its shape.

| Document | Covers | Assumes |
| --- | --- | --- |
| `specs/design-negation.md` | `_` under `not`: meaning, safety, the keyed existence probe, the no-index fallback, explain, the helper-relation property | nothing open |
| `specs/design-errors.md` | the error class: nine classes, the by-construction funnel, the entry point, what the tests read | question 4's list, with INTERRUPTED added for `relic_interrupt()` |
| `specs/design-floats.md` | the future float type in full: syntax, the split number tag, identity equality, exact ordering, arithmetic, the exact `total` accumulator, functions, I/O, determinism | question 1's option 1 |

The lexer change of step 2.1 and the reserved bytes of step 2.2 are
edits to `specs/design-parser.md` rather than a document of their own;
the float design's section 2 states the syntax they reserve. Section 4
holds draft texts for the four decisions.

## 1. What the survey found that the brief did not expect

### 1.1 The fourth type tag is taken (blocks nothing now, shapes the float record)

The brief's 3.2 says a float "takes the fourth and last type tag of D-24"
and asks to record that no tag is left after it. There is no free tag.
`lib/util.h:53-58` assigns 1 = symbol, 2 = string, 3 = integer, and tag 0
is `RELIC_ID_NONE`: it is how a zeroed slot or column says "no value", and
it is the pending marker of a computed value in a batch (`lib/exec.h:322`,
`specs/design-arithmetic.md` section 8.3, "zero type bits are never
assigned to a value"). Using it for floats would break that invariant
everywhere a zeroed column is read as empty.

Options for the record, in order of preference:

1. **Split the number tag's index space.** Tag 3 becomes "number"; the top
   index bit says integer or float, each with 2^29 - 1 distinct values.
   Symbols and strings keep 2^30 - 1. Every consumer of `RELIC_DOM_INTEGER`
   already masks the tag, so the change is local to the integer table and
   the two accessors that split an identifier.
2. Three type bits and 29 of index, halving every table's limit, which
   reverses part of D-24.
3. Move the pending marker to another encoding. It is read in the executor,
   the merge, the measure and the commit; the widest change for the least
   gain.

The float record should say which, and should drop "no tag is left after
it". **Question 1 for you:** is option 1 acceptable to record?

### 1.2 The work-bound item was closed today

The brief's 2.4 says "No limit bounds the work of a join" is in `TODO.md`.
Commit c185817, today, deleted it and documented the gap as a contract in
`man/relic.3:238-241`: a caller evaluating an untrusted program must bound
the time itself. `specs/design-api.md` section 11 also lists a step limit
as deliberately absent, with the reason that adding a configuration field
after 1.0 is an incompatible change.

That reason cuts the other way before 1.0: a `max_work` field is free now
and a major ABI bump later. **Question 2 for you:** reopen it and build it
before 1.0, or leave the contract as written? The design is small either
way (section 2.8), and the unit of work already exists.

### 1.3 Points to settle in the decision texts, not conflicts

* **`$name` against generated names.** D-12 guarantees that no user can
  write a name containing `$`, which is what lets `$q0`, `path$bf`,
  `magic$path$bf`, `sup$...` and `agg$$q0$1` never collide. A `$name`
  parameter is a term, never a predicate, so the parser cannot confuse the
  two, but `-e` output could show `$q0` the relation beside `$q0` a
  parameter. The decision should say a parameter is substituted before
  planning and never appears in a plan, or that generated names keep a
  form (`$` followed by `q` and digits, or an inner `$`) no parameter
  spelling takes.
* **`#name` against the import design.** `specs/design-import.md` section
  3.3 argues that "the grammar stays as it is: a directive would be the
  grammar's first statement about the environment rather than about
  relations". Reserving `#` for a directive must cite the exception
  `docs/todo-language.md` already grants: a rule file naming rule files
  is not a program naming its data.
* **`int(V)` against D-28.** D-28 closes with "`str` is the one conversion
  because it is the one that cannot fail", and `specs/design-strings.md`
  section 19 lists conversions that can fail as deliberately absent. The
  float record must say it amends that sentence: `float(V)` is total,
  `int(V)` fails as integer overflow fails under D-26, an evaluation
  error, not a no-value case.
* **`_` under `not`.** No numbered decision forbids it; the specs derive
  the ban from safety ("nothing can ever bind it", `design-program.md`
  section 4.1, `design-hardening.md:102`, `design-plan.md:409-414`). The
  change is a design change to those documents, not a reversal. The
  constraints example in `docs/todo-language.md:48` already writes
  `not owner(T, _)`.
* **Rejecting `1.2` changes an accepted program's meaning.** Allowed before
  1.0, and the brief's reason stands. The nearest decision, D-16, says one
  integer rule converts literals and the sign is the parser's; the new
  check extends the lexer's existing one-byte lookahead (the `0x10` rule)
  and leaves D-16 intact.
* **Error position.** The existing trailing-byte error points at the
  offending byte (`integer-trailing.dl:1:6` is the `x` of `0x10`), where
  the brief wants the new error at the start of the number. Keep the
  `0x10` message and position as they are, since "unexpected character
  'x' after an integer literal" is the right diagnosis for hex, and give
  `1.2`, `1e5` and `1E5` the new message at the number's start.

### 1.4 Discrepancies to fix along the way

* `specs/design-aggregates.md:1594` says floating point is something
  "`docs/todo-language.md` declines", and `TODO.md:371-372` says that file
  lists "what stays out on purpose with the reason". Commit ed4684a moved
  the declined list to architecture section 3. Both pointers go stale-free
  once the float record exists (section 2.9).
* The brief's 2.5 mentions `-o json`; the format flag is `-F json`, and
  `-o` names an output file.
* `tests/lang/unsafe-anonymous.dl:1-4` and `query-unsafe.dl:1-3` state the
  current `_` rule in comments and need rewording with 2.1.

## 2. The work, in order

Each step is one commit, gated as the working agreement requires: design
text first, then code, then the reviewer pass, then `make check`, with
`check-asan` and `check-ubsan` where the step touches the lexer,
evaluator or allocation. Steps 2.1 to 2.3 and 2.9 are the pre-1.0 set.
Steps 2.4 and 2.5 are recommended before 1.0 as well, because they are
cheaper to ship inside the first release than as a 1.1 change with a
compatibility note. Steps 2.6 to 2.8 wait on your answers.

Decision numbers: the next free one is D-30, since D-29 is the interrupt. The proposal below spends
D-30 on reserved syntax (steps 2.1 and 2.2 together, since both exist for
the same reason), D-31 on `_` under negation, D-32 on the error class, and
D-33 on floats. Warnings as errors and a work bound, if built, each take
the next number. **Question 3 for you:** one decision for 1.1 and 1.2, or
two?

### 2.1 Reject a period directly between two digits (brief 1.1)

**Today.** `scan_integer()` (`lib/lex.c:431-469`) takes the digit run and
fails with `RELIC_LEX_INTEGER_TRAILING` only when the next byte is an
identifier byte. `.` is punctuation, so `1.2` is INTEGER, DOT, INTEGER
with no error; inside an atom the parser then says `expected ',' or ')',
found '.'`, and in a rule body `X < 1.5.` is silently `X < 1.` and a
second statement. `1e5` already fails at the `e` with "unexpected
character 'e' after an integer literal".

**Spec.** D-30 in `specs/decisions.md`: digits, period, digit is reserved
for a float literal and is an error until one exists; `#` and `$` outside
a string are reserved (step 2.2 text); the import reader's integer rule is
unchanged. `specs/design-parser.md` section 3.5 gains the second lookahead
case beside the `0x10` one, and the token table in 3.2 a line for the
error. The "one place the lexer looks past a token" sentence at 3.5 lines
191-196 becomes "the one token".

**Code.** In `scan_integer()`, after the digit run: if the next byte is
`.` and the byte after it is a digit, or the next byte is `e` or `E`
(optionally followed by `+`/`-`) and then a digit, fail with a new
`RELIC_LEX_NUMBER_UNSUPPORTED`, `token->where` left at the number's start.
`report_lex_error()` (`lib/parse.c:522-552`) maps it to
`floating-point numbers are not supported`. `1x`, `0x10` and `1_` keep the
trailing-byte error. The two-byte lookahead reads only within the input
range, as the existing peek does. Nothing else changes: `X = 1. 2 > 1?`,
`X = 1.% c\n2`, `p(1).2 > 1?` all lex as today.

**Tests.**
* `tests/lang`: four rejection tests, each `-c`, exit 1, exact message at
  the number's column: a fact argument `p(1.5).`, a rule body
  `q(X) :- p(X), X = 1.2.`, a query `2.5 > 1?`, and a negative literal
  `p(-1.5).` (the error is at the digits, after the sign). One acceptance
  test that `X = 1. 2 > 1?` and `p(1).2 > 1?` still mean what they did,
  and one that a comment or newline after the period keeps the fact.
* `integer-trailing.dl` or a sibling: `1e5` and `1E5` give the new
  message; `0x10` keeps the old one.
* `tests/unit/test_lex.c`: the new error kind for `1.2`, `1.`, `1e5`,
  `1e+5`; `1.` alone stays INTEGER then DOT. `relic_lex_integer()` cases
  `1.2` and `1e5` as MALFORMED, and `tests/unit/test_import.c` the same
  through an `integer` column. The import reader needs no change: it
  calls `relic_lex_magnitude()`, which refuses any non-digit byte.
* `tests/random`: the generator ends every statement with `.\n` or `?\n`
  and no term spelling contains `.`, so it cannot emit the adjacent form.
  Add the invariant to the program writer's comment and one assertion in
  the harness that the rendered text matches no `[0-9][.][0-9]`, so a
  future term spelling cannot break it quietly.
* `tests/fuzz/relic.dict`: add `"1.5"` and `"1e5"`.

**Docs.** `man/relic.1:68` ("Integers are decimal, may be negative, and
must fit in 64 bits") gains "a period or an exponent directly after the
digits is an error: there are no floating-point numbers". NEWS: a bullet
under the language item.

Size: small. One lexer function, one message, about ten test files.

### 2.2 Record `#` and `$` as reserved (brief 1.2)

**Today.** Both bytes have no class in `byte_class[]` and reach
`scan_punctuation()`'s default, `RELIC_LEX_BAD_BYTE`, "unexpected
character '$'" (`lib/lex.c:785-788`, `lib/parse.c:530-532`).
`bad-byte.dl` pins `$` inside a name; nothing pins `#`, or either byte at
the start of a token in term position.

**Spec.** The D-30 text: `$name` is a value supplied from outside the
program, a parameter, with the generated-name note of section 1.3;
`#name` is a built-in constant or a directive, and a file inclusion, if
designed, is spelled `#include`, with the import-design note of section
1.3. `specs/design-parser.md` 3.1 lines 70-73 gains one sentence naming
the two bytes as reserved, and `docs/todo-language.md:38-45` gains "spelled
`#include`".

**Code.** None.

**Tests.** `tests/lang/reserved-bytes.dl` with both bytes in term
position, as a predicate start and inside a rule body, each line's error
pinned; recovery keeps the test to one file since the parser resynchronizes
at the next terminator. `tests/unit/test_lex.c:744` already has `$`; add
`#`.

**Docs.** `man/relic.1` near line 62: "`#` and `$` are reserved and are
errors outside a string". NEWS: none, no behaviour changes.

Size: tiny. Lands with 2.1 or right after it.

### 2.3 Floats recorded, not built (brief 3)

Documents only; no code. Lands before 2.1 or with it, since D-30 cites the
float syntax as the reason the adjacent form is reserved.

* `specs/design-floats.md` is written. It settles the two choices the
  brief left to the design: `total` accumulates in a 2176-bit fixed-point
  integer and rounds once, so it is exact and a function of the set; and
  `str` uses the plain form for decimal exponents from -5 to 16 and
  `d.ddde[+-]dd` otherwise, always with a period. It records the split
  number tag of section 1.1 as the identifier encoding, and the amendment
  to D-28 for `int()`. It needs a reference from `specs/architecture.md`
  section 3 and section 19.
* D-33 in `specs/decisions.md`: the parts that constrain more than one
  subsystem. A float is a type of its own, equal only to itself (D-11
  stands), ordered exactly against integers, interned by bit pattern with
  negative zero normalized (D-3's one-identifier-per-value stands), no NaN
  or infinity representable, a non-finite result an evaluation error
  (D-26's class), `sum` unchanged and `total` separate (D-27 stands), and
  the amendment to D-28's closing sentence. Says in one sentence that it
  answers the two objections architecture section 3 records.
* `specs/architecture.md` section 3, lines 117 and 130-133: keep floats a
  non-goal of v1, replace "would break D-4 and D-3" with "deferred;
  `specs/design-floats.md` shows how both are kept". Section 6 line 217
  and section 61's `-s` order get a forward pointer, not a rewrite.
* `specs/design-arithmetic.md` section 19, lines 1700-1701: the float
  item points at the design instead of giving the two objections.
* `specs/design-aggregates.md:1594`: the `mean` item points at
  `design-floats.md` (`avg`) instead of the deleted todo-language verdict.
* `docs/todo-language.md`: a "Floating point" entry under "Worth adding",
  Low, pointing at the design; and the `TODO.md:371-372` summary sentence
  updated so it no longer promises a declined list that file does not
  hold. A Post-1.0 bullet in `TODO.md` for floats.

Size: medium in prose, small in risk. The reviewer pass should check the
stub against D-3, D-4, D-11, D-24, D-26, D-27, D-28 by name.

### 2.4 `_` in a negated atom (brief 2.1)

**Today.** The parser only marks `_` (`lib/parse.c:829-830`). Rejection is
the safety check: `compile_literal()` compiles a negated atom's arguments
with `RELIC_POSITION_REQUIRED` (`lib/rule.c:1614-1616`), `compile_term()`
gives such a `_` a fresh slot and adds it to the required set
(`lib/rule.c:577-590`), and nothing binds it, so
`report_unbound_variables()` prints "anonymous variable in rule p/N cannot
be bound by a positive body literal" (`lib/rule.c:1834-1838`). Braces go
through the same `compile_literal()` and report the group variant
(`lib/rule.c:1446-1451`).

Two facts decide the shape of the change:

* The checker fix is one line of intent: compile `_` in a negated atom to
  `RELIC_ARG_WILDCARD` with no slot, as a positive atom does. It then never
  enters the required set, covers braces and queries for free, and the head
  stays rejected because `compile_head()` keeps passing REQUIRED. Named
  variables under `not` stay required.
* **The planner and executor must change in the same commit.** The planner
  places a negated literal only when every column is bound
  (`test_is_placeable()`, `lib/plan.c:1003-1040`; `relic_plan_bound_columns()`
  never counts a wildcard), so relaxing the checker alone yields "plan
  incomplete". Relaxing the planner alone is worse: `place_negated()`
  emits `RELIC_OP_ANTI_MEMBER` with a term per column and `step_member()`
  builds the full tuple with `term_value()`, which for a `RELIC_PLAN_ANY`
  term reads `frame[0]`: wrong answers, no crash.

**Spec.** `specs/design-negation.md` is written: the meaning as an
identity with a helper relation, the third compile position ("probed"),
the probe's three shapes by key mask, the no-index scan fallback, the
explain text and the helper-relation property for the random suite. It
needs a reference from `specs/architecture.md` sections 14 and 15 and
from `specs/design-eval.md` section 5. D-31 states the decision. Document
edits, all design changes rather than status notes: `design-program.md`
section 4.1 (lines 175-188) and
section 5's definition of the required set; `design-plan.md:409-414` and
the invariant at `lib/plan.h:89-91` ("ANY never in a LOOKUP's key"
extends to the anti-probe's key); `design-planner.md` for the new op or
the widened one; `design-hardening.md:102`; `design-aggregates.md:147-148`
("`_` where a positive literal admits it"); `architecture.md:548` (section
14, "named variables of a negated literal"); `design-magic.md` needs no
change, and the commit message says why: negated literals are appended as
they are (`lib/magic.c:2601-2603`), adornments come only from positive
calls, and `renumber_arg()` already carries wildcards through a copy.

**Code.**
* `lib/rule.c`: a third position, or a flag on REQUIRED, so that
  `compile_term()` makes a wildcard for `_` under `not` and in braces. The
  comments at `lib/rule.c:565-576`, `:1751-1754`, `lib/program.h:172-178`
  and `:334-339` describe the new rule.
* `lib/plan.c`: `atom_is_bound()` for a negated literal requires only the
  non-wildcard columns bound. `place_negated()` computes the key mask of
  the non-wildcard columns and, when it is a nonempty proper subset,
  obtains the secondary index as `place_atom()` does through
  `relic_relation_index()`; when every column is bound it keeps today's
  full-tuple find; when the mask is empty it emits an emptiness test. The
  op gains `key_mask` and `index_position` like LOOKUP, or becomes three
  ops; prefer one op with a mask, since the executor's three cases are
  the mask's three shapes.
* `lib/exec.c`: `step_member()`'s anti case builds the probe for the mask
  as `step_lookup()` does (`lib/exec.c:1450-1490`) and asks
  `relic_index_first()` whether a chain exists whose first identifier is
  inside the view; chains are in increasing identifier order, so the first
  entry suffices. Mask zero asks whether the view is empty. Under
  `RELIC_F_NO_INDEX` the positive LOOKUP falls back to a SCAN
  (`lib/plan.c:540-542`); the anti-probe needs the same fallback, a scan
  for the first in-view match, or the no-index strategy of `tests/equiv`
  and `tests/random` diverges.
* `lib/explain.c:915-917`: print the key as LOOKUP does, so `-e` shows
  which columns the probe reads. `explain-ops` and any golden with a
  negated literal will show the change; keep the full-key print identical
  so existing goldens do not churn.
* Streaming (D-22) is unaffected: its "no generator matches `_`" condition
  concerns positive atoms, and `man/relic.3:620` should say "of a positive
  atom".

**Tests.** Write these first and watch the checker tests fail on the
unfixed tree.
* `tests/lang`: `not p(X, _)` in a rule body; `not p(_, _)` on an empty
  and a non-empty relation; `_` under `not` inside braces; a query
  `edge(X, _), not edge(_, X)?` (today's `query-unsafe.dl:8` becomes a
  passing case, so that test keeps only its other lines); the head still
  rejected (`unsafe-anonymous.dl:8`); `_` in a comparison still rejected;
  a negated literal mixing a bound variable, a constant and `_`; one
  through the Magic rewrite with a bound query so the rewritten rule
  copies the literal; one under `-e` to pin the key print. The
  `tests/equiv` suites and the `-X parallel` reruns of `langtest.sh` pick
  these up automatically once listed in `Makefile.am`.
* `tests/unit/test_rule.c:300-342`: extend the scenario "`_` takes a slot
  only where it is illegal" with the negated case taking none.
* `tests/random`: allow `TERM_ANONYMOUS` in `draw_negated()` for bodies
  and groups; relax `term_is_bound()` for negated atoms in the model's
  safety check (it gates generation and shrinking); narrow
  `random_program_has_anonymous()` to positive literals so the derivation
  count stays on. Then the differential property the brief hands over: a
  transformation beside `random_remove_column()` that rewrites each
  `not p(t, _)` to `not p$k(t)` with `p$k(vars) :- p(..., _)` at `p`'s
  level, and a step in `random_compare()` that loads the transformed text
  under the default strategy and compares answers query by query (totals
  differ by the helpers, so answers only). New failure kind, named in
  `main.c`, recorded in `design-hardening.md` 3.1 and 3.4. Helper names
  with `$` cannot be parsed, so the helper is `p_k` under a prefix the
  generator does not otherwise use.

**Docs.** `man/relic.1:65-67` (the `_` sentence), `:360-363` ("every named
variable of a negated literal"), `:350` unchanged; `man/relic.3:620`.
NEWS: a language bullet.

Sanitizers: `check-asan` and `check-ubsan` mandatory; the op touches the
executor and index probes.

Size: medium. The checker is small; the planner, executor, explain and the
random harness are most of it.

### 2.5 An error class a caller can read (brief 2.2)

**Today.** One header, no code or enum. `relic_error()` returns the last
RELIC_ERROR's text, and every error passes through `record_error()` in
`lib/diag.c:58`, the one place `last_error` is stored. Five classes
already have a chokepoint: limits (`relic_diag_limit()`), out of memory
(`relic_diag_no_memory()`, which already tells the memory limit from real
exhaustion), evaluation failures (`report_failure()` in `lib/eval.c:725`
switching on `enum relic_exec_failure`), syntax (`parse_errorf()` and
`report_lex_error()`), and import format (`lib/import.c:1185-1219`). About
sixty sites set the rest directly through `relic_diagf()`: the checker,
I/O, bad arguments, capacity, and some forty "internal error:" sites.

**Spec.** `specs/design-errors.md` is written: the classes, the rule for
a failure two could claim, the funnel that makes an untagged error
impossible to write, the entry point's lifetime, and what the tests read.
It needs a reference from `specs/architecture.md` section 57 and
`specs/design-api.md` section 7. D-32 amends D-5: an error has a class,
read by a new entry point; the text stays the human message and nothing
in the library's contract depends on its wording. The class list, as the
design records it, **question 4 for you**:

| Class | Meaning |
| --- | --- |
| `RELIC_ERROR_NONE` | no error recorded |
| `RELIC_ERROR_REJECTED` | the text was refused: syntax, safety, stratification, import format, a query that does not parse or check |
| `RELIC_ERROR_EVALUATION` | overflow, division by zero, string too long, bad pattern at evaluation |
| `RELIC_ERROR_LIMIT` | a configured limit refused the operation, and the fixed capacities (2^30 symbols or integers, arity, nesting) |
| `RELIC_ERROR_IO` | a file could not be opened, read or closed |
| `RELIC_ERROR_MEMORY` | the allocator refused |
| `RELIC_ERROR_INTERRUPTED` | `relic_interrupt()` stopped the operation |
| `RELIC_ERROR_ARGUMENT` | the caller misused the API: NULL, wrong context, no such query, re-entry from a callback |
| `RELIC_ERROR_INTERNAL` | an invariant the library checks at run time failed |

The brief names the first six. The last three exist in the code and must
go somewhere: the interrupt is reported as its own error today
(`lib/eval.c:1011`), folding misuse into REJECTED would mislead a driver,
and an internal error is the one class a reference implementation should
treat as a bug report.

**Code.** Make the class a parameter of the error path by construction:
`relic_diagf()` with a level goes away; `relic_diag_note()` and
`relic_diag_warning()` take no class and `relic_diag_error(ctx, class,
where, fmt, ...)` requires one, so the compiler finds every site. The
"internal error:" prefix, spelled by hand at some forty sites today, moves
into a `relic_diag_internal()` funnel that supplies prefix and class
together. The five chokepoints pass their class once; the scattered sites
are mechanical edits, each failing loudly if missed. `record_error()`
stores the class beside the text before the callback runs; the re-entry
refusal (`lib/diag.c:30`) answers ARGUMENT. A getter
`enum relic_error_class relic_error_class(relic_t *)`, documented in
`man/relic.3` beside `relic_error()`. The tool's exit status rule (D-5,
0/1/2) is unchanged; whether status 2 should follow the class is a
separate question and not in this step.

**Tests.** `tests/unit/test_diag.c` asserts the class for each chokepoint
and for one site of each scattered kind; `tests/fuzz/fuzz_eval.c:271-277`
stops matching prefixes and reads the class; the exhaustion suite checks
MEMORY against LIMIT where it already tells them apart by text.

**Docs.** `man/relic.3`, `specs/design-api.md`, NEWS library bullet.

Size: medium, mostly mechanical. The by-construction rename is wide and
shallow, which is the working agreement's preferred shape.

### 2.6 Warnings as errors (brief 2.3, optional; needs your go-ahead)

**Today.** Three warnings: the singleton variable
(`report_singleton_variables()`, `lib/rule.c:1903`), the undefined
predicate (`relic_pred_report_undefined()`, `lib/pred.c:540`, swept at the
end of a check and at query prepare, deliberately not incremental because
a later load may define the predicate), and the abandoned Magic rewrite
(`abandon_over_budget()`, `lib/magic.c:896`). Nothing counts warnings; the
tool always prints them and `relic.1:829` says they do not change the
status. `-w` and `-W` are both free.

**Proposal.** A flag bit `RELIC_F_WARNINGS_FATAL` in `relic_config.flags`
(an added interface) and the tool flag `-w`. Under it, a check that
emitted a singleton or undefined-predicate warning fails with class
REJECTED and the message "warnings are errors". The Magic budget warning
does not count: it reports the size of a rewrite, not a mistake in the
program, it arrives at query time where "reject the program" has no
meaning, and the same program passes or fails with it by the `-l`
settings alone. One design point needs care: the undefined sweep runs
when the program is readied, after all loads, so the flag must fail that
check without marking the program invalid in a way a later load cannot
clear. Read `rule.c:2401` and `stratify.c:932` before deciding whether the
existing sticky `invalid` is the right carrier.

Decision: the next free number. Tests: a singleton and an undefined
predicate each under `-w`, exit 1, and the same under `-w -c`; a budget
warning under `-w` still answering. `tests/man.sh` holds the synopsis to
the usage text, so `man/relic.1:12-23`, the usage string and architecture
section 40's synopsis change together.

Size: small.

### 2.7 Mark the language-only tests (brief 2.5, optional; recommended)

**Today.** 207 tests under `tests/lang`, registered by an explicit list at
`Makefile.am:627`. 51 have no `.args`, 43 have exactly `-c` and 29 exactly
`-s`, so 123 depend on no tool feature beyond "check only" and "sort";
84 use `-I`, `-e`, `-X`, `-q`, `-l`, `-i`, `-a`, `-v` or `-F`. Every
`.expected-err` holds relic's exact text, so a conformance run can
compare only status for a rejection test and sorted stdout for an
answering one. There is no manifest and the name prefixes are by topic.

**Proposal.** A manifest, not a naming convention: renaming a hundred
files churns history and the Makefile for no gain. `tests/lang/LANGUAGE`
lists one test per line with a header stating the contract: a listed
test's expectation depends only on the program text and its tables; a
rejection test promises status 1 and nothing about the message; an
answering test promises the sorted rows. `tests/langfiles.sh` checks the
manifest both ways: every listed test exists, has no `.stdin`, and has
`.args` within `{-c, -s}` and no `.csv`/`.tsv` that needs `-I` (a table
is a tool feature; leave those out). The initial list is the 123 filtered
by hand for tests whose point is a tool behaviour (format, verbose,
stats), which the prefix suggests but does not decide.

No decision number; this is test infrastructure. `specs/design-release.md`
or `architecture.md` section 72 gets one sentence naming the manifest.

Size: small, mostly review of 123 tests.

### 2.8 A bound on work (brief 2.4, optional; see question 2)

If reopened: a `max_work` field in `relic_config_t`, a `-l work=N` key in
`limit_named()` (`cmd/relic/main.c:775`), counted in the unit that already
exists, `relic_exec_work()` = derivations + tuples scanned + tuples looked
up (`lib/exec.h:187`), which langtest.sh already proves identical across
thread counts. Checked where the counters are already gathered: after each
executor batch in `take_batch()` (`lib/eval.c:945`) on the serial path, and
after the merge in `take_worker_counters()` (`lib/round.c`) in a parallel
round, so the refusal is deterministic and the same at every `-j`. Refused
as a limit through `relic_diag_limit()`, class LIMIT. Amends D-7's
"the limits bound what is stored" and `design-api.md` section 11; the
`relic.3:238-241` contract becomes "unless `max_work` is set". The eval
fuzz target can then drop `-timeout` as its only guard.

Size: small to medium. The only subtlety is that the work counted before
the check lands in `relic_stats_t`, so the refusal is "after at most one
batch past N", which the man page states.

### 2.9 Order and gates

| Step | Brief | When | Decision | Size |
| --- | --- | --- | --- | --- |
| 2.3 floats record | 3 | before 2.1, docs only | D-33 | medium prose |
| 2.1 reject `1.2` | 1.1 | before 1.0 | D-30 | small |
| 2.2 reserve `#` `$` | 1.2 | with 2.1 | D-30 | tiny |
| 2.4 `_` under `not` | 2.1 | recommended before 1.0 | D-31 | medium |
| 2.5 error class | 2.2 | recommended before 1.0 | D-32 | medium |
| 2.7 manifest | 2.5 | any time | none | small |
| 2.6 warnings fatal | 2.3 | on your word | next | small |
| 2.8 work bound | 2.4 | on your word, before 1.0 if at all | next | small |

Every step: reviewer agent first with the security and readability
framing, then `make check`; `check-asan` and `check-ubsan` for 2.1, 2.4,
2.5 and 2.8; `make check-random` with a few thousand programs after 2.4.
NEWS is one unreleased section, so each step edits the 1.0.0 bullets
rather than adding a release.

## 3. Questions, collected

1. Floats: record option 1 of section 1.1 (split the number tag's index
   space) in place of "the fourth and last tag"?
2. Work bound: reopen the item closed today and build it before 1.0, or
   keep the documented contract?
3. One decision number for 1.1 and 1.2 together, or two?
4. The error class list of 2.5, in particular the three classes the brief
   does not name, INTERRUPTED, ARGUMENT and INTERNAL.
5. Build 2.6 (warnings as errors)? Build 2.7 (the manifest)? I recommend
   2.7 and have no strong view on 2.6.
6. Do 2.4 and 2.5 go before 1.0, as recommended?

## 4. Decision drafts

Texts for `specs/decisions.md`, in its format: an `### D-N Title` heading
and prose, with the reversal or amendment named in a sentence and the
specifying document named last. Numbers follow question 3's answer.

### D-30 Syntax reserved for extension

Three forms the lexer rejects are reserved rather than merely absent, so
that a later version, or a language built as a superset of this one, can
give them a meaning without changing what any accepted program means.
Digits, a period and a digit with nothing between them are a
floating-point literal, rejected until the type exists; a period after
digits followed by anything else is the terminator it is today, so the
lexer's one-byte lookahead after an integer becomes two. `$` followed by
a name is a parameter, a value supplied from outside the program; the
generated names of D-12 keep `$` where no parameter spelling can stand,
so the two never meet in a plan or a diagnostic. `#` followed by a name
is a built-in constant or a directive, and a file inclusion, if designed,
is spelled `#include`; `specs/design-import.md` section 3.3's argument
against a program naming its data stands, since a rule file naming rule
files is not that. Nothing else is reserved: `true`, `false` and `null`
stay symbols and no word becomes a keyword. D-16's one integer rule is
unchanged. Specified in `specs/design-parser.md` section 3.5 and
`specs/design-floats.md` section 2.

### D-31 `_` in a negated literal is a column the literal does not read

A negated literal holds exactly when no tuple of its relation agrees with
it on the columns that are not `_`; each `_` is quantified inside the
negation, `not p(_, _)` tests emptiness, and the literal is equivalent to
negating a helper relation projected onto its named columns. Such a `_`
is a wildcard that takes no slot, as in a positive literal, so the safety
invariant of `specs/design-program.md` section 4.1 is unchanged; a named
variable of a negated literal must still be bound, and `_` in a head, in a
comparison or as an aggregated variable is still rejected. The literal is
an existence probe keyed on its named columns, through the canonical
index when every column is named, a secondary index built on demand when
some are, and the relation's emptiness when none is. Stratification and
the Magic rewrite are untouched: both read the literal's predicate and
not its arguments. Specified in `specs/design-negation.md`.

### D-32 An error has a class

Every RELIC_ERROR carries one of a small enumeration of classes that says
what the caller can do about it: the text was rejected, the evaluation
met a result the language leaves undefined, a limit refused, a file
failed, the allocator refused, the caller interrupted, the caller misused
the interface, or an invariant failed. `relic_error_class()` returns the
class of the error `relic_error()` returns the text of, with the same
lifetime, and may be called from a callback. The class is attached by
construction: an error can be reported only through a function that
takes one. Amends D-5, whose return convention, diagnostic levels and
exit statuses stand; the text remains the interface for a person and
nothing in the library's contract depends on its wording. Specified in
`specs/design-errors.md`.

### D-33 Floating point, when added, is a fourth type equal only to itself

A float is an IEEE 754 double and a type beside symbol, string and
integer, interned by bit pattern in a table of its own with negative zero
normalized, so D-3's one identifier per value holds; its identifier is
tag 3 of D-24 with the top index bit set, which leaves 2^29 - 1 distinct
integers and as many floats and moves no other limit. No NaN and no
infinity can be written, imported or computed: a result that is not
finite is an evaluation error of D-26's class. An integer never equals a
float, which the assignment proof of `specs/design-arithmetic.md` section
4.5 requires, so D-11 stands; an integer and a float order exactly by
their mathematical values. `sum` is unchanged and `total` sums in
floating point exactly and rounds once, so D-27's function-of-the-set
promise extends to it. `float()` is total and `int()` fails as an
overflow fails, which amends D-28's closing sentence: `str` and `float`
are the conversions that cannot fail, and `int` is the one that fails as
arithmetic does. Determinism rests on `FLT_EVAL_METHOD` 0, no
contraction, no fast math, relic's own correctly rounded parser and
printer and no libm function, so D-4 stands. Architecture section 3's
two objections are answered by these mechanisms, and floats stay out of
version 1. Specified in `specs/design-floats.md`.
