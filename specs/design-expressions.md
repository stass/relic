# Expressions

This document covers the machinery that arithmetic and the string functions
share: how an expression is held in a compiled rule, how the planner turns
it into a program the executor runs, what a value the executor computes is
while a plan runs, how such a value reaches a stored tuple without the
executor writing to the context, and the regular expression matcher behind
`match`. It is an extension of the pipeline that spans the front end, which
parses an expression into a tree, planning, and the evaluator, which takes
the executor's batches. What an
expression means -- the operators, the functions, which operand types give
no value and which results are errors -- is the language's business and is
stated in [language.md](language.md#expressions-and-assignment); this
document says how that meaning is carried out.

Related: [language](language.md#expressions-and-assignment),
[functions](language.md#functions),
[regular expressions](language.md#regular-expressions),
[evaluation errors](language.md#evaluation-errors),
[architecture](architecture.md#expressions),
[front end](design-frontend.md), [values](design-values.md),
[planning](design-planning.md), [evaluation](design-eval.md),
[parallel evaluation](design-parallel.md), [Magic Sets](design-magic.md),
[aggregates](design-aggregates.md).

## Model

An expression passes through four forms:

1. **The parse tree**, built by the parser from the text, a transcription
   of what was written (see [design-frontend.md](design-frontend.md)).
2. **The expression tree** of the compiled rule: constants resolved to
   domain identifiers, variables to slots, functions to the entries of one
   function table. It lives in the arena of the rule that holds it and as
   long as that rule.
3. **The postfix program** of a plan: the tree flattened into a sequence of
   steps, built when the plan is built, with every decision that depends on
   the plan -- which kind of slot each read is -- written into the steps.
4. **Cells** on the executor's stack while the program runs, each holding a
   domain identifier, a computed integer or a computed string.

Two plan operations run programs. A **filter** runs two and compares the
cells they leave; an **assignment** runs one and binds a slot to the cell it
leaves. Both are tests in the planner's sense: they read bound values, match
at most once, and never enumerate.

A value the executor computes has no domain identifier while the plan runs,
and may never get one. It stays in the scratch -- an integer in the frame's
number array, a string in the text stack -- and is interned only if a head
tuple holding it is stored. The data flow for a computed value is:

```text
assignment  ->  number or text slot  (+ read-only probe for an identifier)
            ->  filters, keys and later expressions read it from the slot
            ->  emit: identifier if the probe found one, else a pending marker
            ->  batch taken by the evaluator: pending values interned, batch inserted
```

## Invariants

* **The executor never writes the context.** Every lookup a plan makes of
  the value table or the symbol pool is read-only. Interning happens on the
  evaluator's side, after the executor has handed a batch back, and in a
  shared round, one whose work is divided among threads
  ([parallel evaluation](design-parallel.md#model)), only in the merge's
  last phase.
* **A slot's kind is static.** Each slot of a plan is a domain slot, a
  number slot or a text slot, fixed by the operation that binds it. Every
  read of the slot is compiled knowing the kind, so the executor never asks.
* **Zero in the frame means "no identifier".** A computed value the context
  has not assigned has frame entry zero, which no tuple column and no index
  key holds.
* **No pending marker leaves the evaluator.** A batch holding a marker is
  resolved before it is hashed, screened, inserted or delivered.
* **Every walk is bounded.** The parser bounds an expression tree's height,
  the pattern compiler bounds a pattern's parenthesis depth, and the
  executor's stack size follows from the first; nothing recurses on input
  without a bound.

## Expression trees

A compiled comparison holds two expression trees; a term is a one-node tree.
A node is a constant, a variable, a negation, a binary operator or a call of
a function with up to three arguments. Each node records its height and the
number of nodes under it, so that a consumer can size what it needs, and a
limit can charge what a tree costs, without a walk of its own.

**The tree is kept, rather than compiled straight to the postfix form,
because three consumers need its shape:**

* **explain** prints a rule from its compiled form, and must print
  `(D + 1) * 2` with the parentheses the tree requires and a call as the
  call it is;
* **the Magic rewrite** copies comparisons into the prefixes of the rules
  it drafts, renumbering their slots by walking the trees, and compares
  generated rules structurally to drop duplicates;
* **the planner** reads which slots an expression mentions, and builds the
  postfix program only once the plan has fixed the kind of every slot.

The postfix program is a plan's, not the rule's, precisely because of the
last point: the kind of a slot a program reads depends on the order the
planner chose (see [design-planning.md](design-planning.md#slot-kinds)
for the `p(X), q(Y), X = Y` case, where the same rule gets a different slot
kind under a different order).

**Constants are interned when the rule is compiled.** The rule compiler
turns every constant in a statement into a domain identifier, an
expression's constants included -- the `1` of `X + 1` is interned exactly
as the `1` of `p(1)` is. A program that never stores a computed `1` has
interned an integer it does not need; the cost is one table entry, and the
benefit is one rule for constants instead of two.

**A constant pattern is compiled with the rule.** When the second argument
of `match` is a string constant, the rule compiler compiles the pattern
once, into the rule's arena, and the call node keeps the compiled program.
A pattern that is not a regular expression is then a check error at the
literal, reported before anything is evaluated, so nearly every bad pattern
is found at load time. The compiled program lives as long as the rule; a
plan's step points at it, and a rule the Magic rewrite copies keeps the
pointer, since the arena holding the rule outlives every rule generated
from it.

**Function signatures live in one table** (`lib/strfn.c`): each function's
name, arity, argument kinds and result kind. The parser checks calls
against it, the planner reads a call's result kind from it, and the
executor checks argument kinds through it before switching on the
function, so no consumer carries its own list of
what `substr` takes.

The parser enforces the two nesting bounds, one on the descent of the text
and one on the height of the tree; why there are two is explained in
[design-frontend.md](design-frontend.md). Only the height matters below:
every walk over a tree, and the executor's stack, is bounded by it. The
number of nodes is not bounded, only the height; the length of the text is
what bounds the node count, as it bounds a body's length.

## Postfix programs

A program is its tree's nodes in postfix order, each operand's steps before
its operator's, so that running the steps in order over a stack leaves the
value as the one remaining cell. The step roles are:

* push a constant's identifier;
* push a slot, as a domain identifier, a computed integer or a computed
  string, the step saying which by the slot's kind;
* pop one integer and push its negation;
* pop two integers and push the result of a binary operator;
* pop a function's arguments and push its result; a `match` whose pattern
  was a constant carries the compiled pattern.

The program is chosen over walking the tree at run time for the reason plans
are data (see [design-planning.md](design-planning.md)): an array that
states every decision in advance, run by a loop with no recursion and no
branch on what kind of slot is being read. The step and program types are
in `lib/plan.h`.

**The stack's depth is computed per program and bounded by the tree's
height.** A leaf needs one cell. A node holds each earlier argument's cell
while it evaluates a later one, so the cells a node needs are the greatest,
over its arguments counted from zero, of the argument's own need plus its
index. With at most three arguments, each level of the tree adds at most two
cells, so a tree of height *h* needs at most 2*h* - 1 cells -- `substr`
nested in its own third argument is the shape that reaches the bound. The
walk that emits a program records its need, and the executor's stack is
sized once for the greatest height the parser admits (`lib/plan.h` states
the bound and asserts the arity it rests on).

**Fixed arity is what keeps that bound.** A variadic `cat` would need a cell
per argument, and therefore either a second bound on the argument count or
a stack sized per plan. `cat(cat(A, B), C)` costs the writer a few
characters and the design nothing.

## Slots and cells

An assignment binds a domain, number or text slot by the kind of its
expression's result; what each kind holds and why the plan, not the rule,
fixes it is [design-planning.md](design-planning.md#slot-kinds)'s. A text
slot names its string by offset and length in the text stack.

A number or text slot also has a frame entry: the value's identifier when
the context has assigned one, found by **one read-only probe** of the
integer table or the symbol pool at the moment the slot is bound, and zero
otherwise. That probe is what lets every later reader stay generic:

* a **generator or membership test** keyed on the slot builds its key from
  the frame entry. Zero is in no column and no index key, so a value the
  context lacks matches no tuple -- which is correct, because a value the
  context has never assigned is in no relation. A negated literal over
  such a value therefore succeeds, as it must;
* an **expression** reading the slot pushes the computed value itself,
  never the frame entry, so a value that was never interned is still
  computed with;
* an **emit** writes the frame entry when it is not zero, and a pending
  marker when it is (see
  [Computed values in a head tuple](#computed-values-in-a-head-tuple)).

The probe costs a hash and a lookup per assignment. It is the price of the
executor staying a reader; an assignment that interned its result would
write the context, which a task of a shared round cannot do.

A **cell** holds one of three things: a domain identifier, a computed
integer, or a computed string. An operator or a function reads each
argument cell as the type it wants: a computed integer or string is itself;
an identifier is decoded through the integer table or to its bytes in the
pool. A cell that does not decode to the wanted type is **no value**, which
ends the run without a value; the operation that ran it rejects the binding
and records the mixed-type note. A result that is not a value -- overflow,
a zero divisor, a string longer than a value can hold, an invalid variable
pattern -- ends the run with an **error** instead. The distinction is the
language's (see [evaluation errors](language.md#evaluation-errors)); here
it is two outcomes of a run, carried to the caller by different records.

Comparing two cells follows the language's comparison rules: two
identifiers compare by identifier for equality and by decoded value for
order; two computed values of one type compare by value; a computed value
and an identifier compare by decoding the identifier. A computed value is
never interned to compare identifiers, since that would write the pool.

**Arithmetic is checked before it computes.** Sums, differences and
products use the compiler's checked builtins where `configure` finds them,
and otherwise compare the operands against the bounds before computing;
both forms are built and run under the same unit tests. Division and
remainder test for a zero divisor and for `INT64_MIN / -1` before dividing,
so no instruction the hardware traps on is ever issued, and there is no
signed overflow for UBSan to find (`lib/arith.c`).

**`substr` never forms an out-of-range value.** It intersects
`[I, I + N)` with the string's positions without ever computing `I + N` or
`-I`: a start at or past the length, or a length at most zero, keeps
nothing; a negative start keeps nothing when `I <= -N` (defined because
`N` is then positive), and otherwise moves the start to zero and shortens
`N` by `-I`, a sum of a positive and a negative integer that cannot
overflow; from the start so fixed, at most the remaining bytes are kept.
Each step stays inside `int64_t`, so the clamping the language defines over
the integers holds at `INT64_MIN` and `INT64_MAX` as well.

`index` is `memmem(3)`, which FreeBSD, macOS and glibc provide;
`configure` selects the replacement in `compat/` where it is missing, as
the [dependency policy](architecture.md#dependency-policy) asks.

## The text stack

A computed string lives in the scratch's **text stack**, a byte array, and a
cell or a text slot names it by offset and length. A function whose result
is a string builds it at the top of the stack: `cat` copies its operands;
`substr` of a stored string copies the range it keeps, and of a computed
one names the kept range where it already lies; `str` formats an integer
in decimal, as the output formats print it, copies a stored spelling, and
returns a computed string as itself. A result longer than a string value
can be is the error it is before a byte of it is copied.

The stack is a stack of strings, not of frames: nothing is saved on descent
or restored on backtracking, for the same reason nothing in the frame is.
What keeps a string alive is a rule about where each operation builds:

> Each operation builds the strings its programs compute above the string
> of the last text-binding assignment placed before it in the plan, or from
> the bottom when there is none.

The planner records that position in every filter and assignment. An
assignment that binds a text slot moves its result down to the base it
built above, so its temporaries are given back, and records the top just
past its string; an assignment whose result lies wholly below its base --
another text slot's string, or a part of one -- binds the slot to it where
it lies and records its base as the top. Every other operation's strings are
dead the moment it finishes, because nothing records a top above its base.

**Why every read is safe.** A text slot an operation reads was bound by an
assignment at or before the last text-binding assignment preceding that
operation, so its string lies at or below that assignment's recorded top,
and the operation builds only above it. A string is overwritten only by an
operation placed before the assignment that wrote it, one whose base lies at
or below it; such an operation runs again only after a generator before that
assignment produced a new binding, and the assignment then rebinds the slot
before anything after it reads it. So no string a live slot names is ever
overwritten, and no temporary outlives the operation that built it,
whatever kind of operation it was or however it ended (`lib/plan.h`,
`lib/exec.c`).

A plan that calls no string function never touches the stack.

## Round-scoped buffers

Most of a scratch is sized from the plans when an evaluation begins, and is
the same at every thread count. Four buffers are sized by the data a plan
meets instead: the text stack, the batch's pending bytes, the room for a
variable pattern's compiled program, and the matcher's work space. Each
grows by doubling until a request fits, so its capacity is a monotone
function of the largest request made of it, and each growth is charged to
the memory limit before it happens.

**These four are round-scoped in every mode.** When a round begins the
text stack and the pending bytes are allocated at their initial sizes, and
the program room and work space start empty; all four are released, with
their charge, when the round ends -- whether the round ran on the serial
evaluator's scratch or on the workers', and whether it was merged,
declined or abandoned. A query's plan run outside any round owns its
buffers the same way. A worker's buffers
are released when its task finishes, before the merge asks the memory limit
whether the round fits.

The point of the scoping is that **the charged total at the start of every
round is the one a serial run would hold**, so the memory limit's decisions
do not depend on how many threads ran the rounds before. How a shared
round's merge admits these buffers, never more laxly than the serial run,
is [memory](design-memory.md#growable-arrays)'s. The cost is one small
allocation per buffer per round.

## Computed values in a head tuple

An emit writing a head column from a number or text slot whose frame entry
is zero has no identifier to write. It appends the value to the batch's
**pending** array -- an integer, or a range in the batch's pending bytes
for a string -- and writes a **pending marker**: the entry's position,
counting from one so it is never zero, with
type bits of zero. No assigned identifier has zero type bits, so nothing
that decodes identifiers can mistake a marker for a value. One array and
one marker space serve both kinds, so resolving a batch is one loop over
its columns.

The pending array has room for every column of a full batch, so no emit
allocates an entry. The pending bytes cannot be sized in advance, so a
batch has a second way to be full: an emit that finds its tuple's pending
strings do not fit returns the batch to its caller, as it does when the
batch holds its full count of tuples, and is asked again on the same
bindings once the caller has resolved and emptied the batch. Only a single
tuple too large for an empty batch grows the buffer. No tuple is written
twice or skipped.

**Resolution is the evaluator's.** When it takes a batch, the evaluator
measures the pending values (the identifier each has or is promised, and
the bytes the tables grow by to hold the new ones), asks the memory limit,
rewrites the markers, commits the new values, and only then inserts. A new
value's identifier is predictable from the table's count and the order
values are met, which is what lets a shared round's merge decide
everything before it changes anything. The mechanism -- measure, rewrite,
commit, the stepwise capacity, and why identifiers come out the same at
every thread count -- is described once, in
[design-values.md](design-values.md).

A query whose plan computes anything, or folds an aggregate, is gathered
into its answer relation rather than streamed: such a plan can fail on any
binding, and a query that fails must deliver no row (see
[design-eval.md](design-eval.md)). So a streamed batch never holds a
pending marker, and "every batch is resolved before it goes anywhere" needs
no exception for the one destination that stores nothing.

## Notes and errors cross the executor's boundary

The executor records rather than reports, and its caller reports when it
takes the batch; how the two records cross, when the note is reported and
how a shared round's error is reported in serial order are
[evaluation](design-eval.md#notes-and-errors-cross-the-executors-boundary)'s.
What expressions contribute:

* the **note** for a value of the wrong type: an operand of an operator, an
  argument of a function, a row of an aggregate. The run ends with no value
  and the operation rejects the binding;
* the **error** for a result that is not a value: overflow, a zero divisor,
  a string longer than a value can hold, or an invalid variable pattern,
  recorded with where it happened and which failure, including which
  pattern failure.

## The regular expression matcher

`match` is decided by relic's own matcher (`lib/regex.c`, `lib/regex.h`).
The dialect -- POSIX extended expressions over bytes without counted
repetition, classes, backreferences, captures or flags -- is defined in
[language.md](language.md#regular-expressions); this section describes how
it is decided.

**Why not `<regex.h>`.** It is a libc interface, so it would add no
dependency, but glibc, macOS and FreeBSD each ship a different engine. They
disagree in the edge cases POSIX leaves undefined, on bytes above 0x7f, and
on what a NUL in the subject does; some take exponential time on patterns
an untrusted program can write; and each compiles under the process's
locale, which relic never sets. A `match` whose answer depended on the
platform would break [determinism](architecture.md#determinism) and every
golden test with it. A vendored engine would fix that at the audit cost of
tens of thousands of lines, where a matcher written to this dialect is a
few hundred. Because `lib/` is on the include path, `#include <regex.h>`
anywhere in the tree resolves to relic's own header, so the platform engine
cannot be reached by accident.

**Compilation** is one recursive descent over the pattern, with parenthesis
nesting bounded so that the descent is, into an array of instructions:
consume a given byte, any byte, or a byte of a 256-bit set; split to two
targets; jump; assert the start or the end of the string; match. Each
construct emits its instructions as it is parsed -- a repetition inserts
its split before an operand already emitted, moving the operand up by one
-- and a run of repetition marks after one operand is read whole and
emitted once. No construct emits more than three instructions per byte of
pattern, so a pattern's program size is known before compilation begins.
That bound is what lets a constant pattern's program be laid out in the
rule's arena in one allocation, and a variable pattern's in the scratch's
program room, grown once. A pattern of any bytes compiles to a program of
known size or to one of the dialect's errors, with the byte where the
compiler found out.

**Matching is Thompson's simulation, run to find the least position at
which a match begins.** The state is a list of threads per position of the
subject, each thread an instruction and the position its match began, kept
in nondecreasing order of start:

* at each position, while no match is found, a thread starting there is
  added last, which keeps the order, since every thread already on the list
  began earlier;
* stepping over a byte appends each surviving thread's successors in order,
  following splits, jumps and anchors at once, and adds an instruction only
  if the list does not hold it yet. The thread that gets an instruction is
  therefore the earliest-starting one to reach it -- the only one the
  leftmost match can come from;
* a thread reaching the match instruction records its start if it is the
  least found. After that no new start is added, and threads starting at or
  after the answer are dropped; the run ends one position past the
  subject's end, so an empty match at the end and `$` are found, or
  when no thread remains.

A list holds each instruction at most once, so each position costs at most
one visit per instruction, and a match runs in time proportional to the
subject's length times the program's, whatever the pattern: `(a*)*b` over a
million `a`s takes a million times a few steps, where a backtracking matcher
would not finish. Only the start of the match is reported, so no priority
rule among matches beginning at one position and no submatch tracking is
needed, which is most of why the matcher is small. A program that must
begin by consuming a byte of some set lets the search skip straight to the
next position holding such a byte while no thread is alive.

**Program and work space.** A compiled program is
immutable: the matcher reads it and writes nothing into it. Everything
mutable -- the two thread lists and a mark per instruction -- is a **work
space** owned by whoever runs the match, one per executor scratch. That is
what lets a constant pattern's program sit in the rule's arena and be run
by every worker of a round at once, each with its own work space. The marks
record the step that last added each instruction, so a new step empties
them by taking a new stamp rather than clearing them, and a short subject
against a large program does not pay for the program's size per byte. The
matcher neither recurses nor allocates.

A **variable pattern**, one that arrives in a column, is compiled when the
call runs, into the scratch's program room, run once, and discarded; a rule
that matches a million rows against the pattern in a column compiles it a
million times. A pattern that does not compile is the evaluation error for
an invalid pattern, met on the binding that supplies it.

## Magic Sets

The rewrite (see [design-magic.md](design-magic.md)) takes a rule's body in
the planner's order, and needs to know what each literal binds. One function
answers that for both the planner and the rewrite: a positive literal binds
its variables, an equality that assigns binds its variable, an aggregate
literal binds its result. So a call after `D1 = D + 1` whose argument is
`D1` is adorned bound, and a supplementary predicate's columns include
variables an assignment in the prefix bound -- the computed value is
interned when the supplementary tuple is stored, like any other.

Comparisons are copied into prefixes as they are, by walking their trees to
renumber slots, and hashed and compared structurally when generated rules
are deduplicated; a copied `match` keeps its compiled pattern. Everything
downstream plans and runs a generated rule without knowing what it holds.

**A literal's size counts its nodes.** The rewrite's work limit counts
literals; an expression of a thousand nodes copied into ten prefixes is ten
thousand nodes of copying and walking that a count of literals would not
see. So the size the limit uses for a comparison is one plus the nodes of
its two trees, the count each node records, and a rewrite over a program of
large expressions stays a constant multiple of that program's size.

## Failure paths and limits

Expressions and strings give the writer of an untrusted program three
tools; each is taken away:

* **Undefined behaviour.** Arithmetic is checked before it computes.
  `substr` clamps without forming an out-of-range value; every function
  reads only inside the ranges it was given and checks a result's length
  before copying a byte. The parser's descent, every tree walk and the
  pattern compiler are bounded; the executor and the matcher do not
  recurse. No expression and no pattern can overflow the C stack, however
  shaped and whether or not well formed.
* **Unbounded memory.** A rule that computes a new value per derivation
  grows the integer table or the symbol pool, and both are charged to the
  memory limit whole (see [design-values.md](design-values.md)). The four
  data-sized buffers are charged as they grow. A variable pattern's program
  room and work space are proportional to its length -- a few hundred bytes
  of charged scratch per pattern byte at worst, for the duration of one call
  -- and the limit sees them. A large expression copied by the rewrite is
  charged to the rewrite's limit by its size. Without a memory limit, such
  a program runs until the tuple limit, a table's count limit or exhaustion
  stops it, which is the position every unbounded recursive program is in.
* **Unbounded time.** `match` is linear in subject times pattern by
  construction. `index` is at worst the product of its operands' lengths,
  as `memmem(3)` is, and `cat` is linear in its result. A recursive rule
  that computes, such as `n(Y) :- n(X), Y = X + 1`, need not terminate;
  termination is the program's responsibility (see
  [language.md](language.md#expressions-and-assignment)), and the limits
  bound what is stored, not what is computed. A caller that needs a time
  bound interrupts the evaluation.

Nothing an expression computes reaches a caller unformatted: a computed
integer prints as an integer and a computed string as a string, with the
escaping every format gives one. No diagnostic quotes a computed value, so
none can carry text a program built; a quoted pattern is the constant the
program wrote, escaped as explain escapes it.

The integer and pool count limits, the string length limit and the nesting
bounds are user-visible and listed in [language.md](language.md#limits).

## Determinism

* An expression's value is a function of its operands, computed in checked
  64-bit integer arithmetic and over bytes, with no floating point, locale or
  character set involved, so it is the same on every platform. `match` is
  decided by relic's own matcher for the same reason.
* Where assignments and filters are placed is a function of the source and
  the strategy, so it is the same on every run.
* Every strategy that succeeds gives the same answer; whether a strategy
  meets an evaluation error is a function of the program and the strategy
  (see [language.md](language.md#evaluation-errors)).
* Computed values are interned in serial derivation order at their first
  stored occurrence, whatever the thread count, so identifiers and value
  counts are identical at every thread count (see
  [design-values.md](design-values.md)). Nothing orders values by
  identifier in any case.
* An evaluation error is the first in serial order, at every thread count,
  because the serial evaluator always reports it.
* Which rule's mixed-type note comes first may depend on the strategy;
  nothing else about the diagnostics does.
* Explain prints an expression from its tree with the parentheses the tree
  requires, so equal trees print equal and the printed text parses back to
  the tree (see [design-planning.md](design-planning.md)).

## Alternatives not taken

* **Expressions in heads and atom arguments.** Such an expression is sugar
  for a fresh variable and an assignment; the compiled rule would hold a
  variable the writer never wrote, explain would show a rule that is not
  the one in the file, and the singleton check and a query's output columns
  would each need a rule for it.
* **Interning computed values in the executor.** It would make the
  executor a writer of the context, which the frozen store of a shared round
  forbids by construction, and would leave a declined merge with values to
  undo.
* **A second table for computed values.** The same integer or string
  computed by a rule and written in a later load would get two identifiers,
  unless every interning consulted both tables in a fixed order. One table
  per type gives one identifier per value by construction.
* **Unboxed integer columns.** Storing a computed integer inline in a tuple
  would remove the pending mechanism but give one integer two encodings,
  which the value representation rejects (see
  [design-values.md](design-values.md)).
* **A cache of compiled variable patterns.** A pattern in a column is
  compiled per binding, in time linear in the pattern, about what matching
  one byte of the subject can cost; a cache would add state shared by every
  worker of a round, and an eviction policy, to save a constant factor.
* **Constant folding.** It changes no answer, and an expression of
  constants alone costs one evaluation per binding, which its writer can
  fold by hand.
