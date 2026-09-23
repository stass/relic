# relic program model design

**Status.** Design document for the program model: predicate identity,
variable numbering, the compiled form of a rule, the safety analysis and the
warnings that go with it. Forward-looking, like every document under `specs/`;
`STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 7, 14, 15, 17, 25, 51 and 63;
`specs/design-parser.md` for the tree this consumes; `specs/design-memory.md`
for the arenas and the interning tables; `relic-plan.md` decisions D-4, D-5,
D-6, D-7 and D-12.

---

## 1. What this document settles

* what identifies a predicate, and where the table of them lives;
* what a variable is once a rule has been compiled, and what numbers its
  occurrences share;
* what a compiled rule looks like, and why it is not the syntax tree;
* what makes a rule safe, stated as one set computation rather than a search;
* which mistakes are errors, which are warnings, and what each one says;
* when validation runs, what it caches, and what a later load does to that.

It does not cover the dependency graph, stratification, planning or
evaluation. Those read the compiled program this document defines and are
settled in `specs/design-stratify.md`, `specs/design-planner.md` and
`specs/design-eval.md`.

---

## 2. Shape of the middle end

```text
relic_ast_program_t -> relic_check() -> relic_program_t
```

Two modules, with the predicate table underneath the rule compiler because
compiling a rule is what discovers predicates.

**`lib/pred.c` decides what a predicate is.** It interns `(name, arity)` pairs
to predicate identifiers, records where the program first mentioned each one
and whether anything defines it, and formats a predicate for a diagnostic.

**`lib/rule.c` decides what a rule is.** It numbers a rule's variables,
resolves its atoms against the predicate table, checks safety, warns about
singletons, and builds the compiled rule the later stages plan over. It also
owns `relic_check()`, which drives the whole pass and is the only entry point
into it.

The syntax tree is not modified. It is the record of what was written, and a
diagnostic that quotes a location or a spelling reads it after the compiled
program exists.

---

## 3. Predicate identity

A predicate is `(name, arity)` and nothing else, per architecture section 17.
`foo/1` and `foo/2` are unrelated, so no arity-mismatch diagnostic exists in
this language: two different arities are two different predicates rather than
one predicate used inconsistently.

Predicates are interned exactly as symbols and values are, with the same table
from `specs/design-memory.md` section 6 and the same two-step reserve and
place. The key is the pair, hashed as one 64-bit word built from the interned
name and the arity, so no new hash function is introduced and the probe order
is fixed at compile time like every other one.

An entry records:

```c
struct relic_pred {
	relic_location_t	where;	/* first mention in the program */
	uint32_t		name;	/* interned */
	uint16_t		arity;
	bool			defined;
	bool			reported_undefined;
};
```

`defined` becomes true when a statement's head names the predicate, whether
that statement is a fact or a rule. A predicate may have both, per section 17,
and `defined` says only that something derives into it.

**The table is context-lifetime state, not derived state.** It grows as loads
introduce predicates, entries are never removed and identifiers are never
reused, which is what makes a predicate identifier usable as a stable index by
every later stage. That matters from S4 onward, where the relation holding a
predicate's tuples hangs off its entry and must outlive any one query.

Identifiers are assigned in first-appearance order per D-4. Within a
statement, the head is interned before the body and the body in textual order,
so a program always produces the same numbering.

**Every atom in the program is interned, including a query's.** Predicate
identity is a property of the program rather than of a rule, and the
undefined-predicate warning below is about the program. Compiling a query into
a rule is a separate question, settled in section 9.

---

## 4. Variables and slots

A variable belongs to one rule, per architecture section 7. Compilation
assigns each distinct variable of a rule a **slot**: a small integer that
indexes the binding frame of architecture section 25, so the evaluator never
sees a name.

Slots are assigned in order of first occurrence, reading the head and then the
body in textual order. `path(X, Z) :- edge(X, Y), path(Y, Z).` numbers `X` 0,
`Z` 1 and `Y` 2: the statement as written, which is the order a reader can
reconstruct without knowing anything about the compiler.

Occurrences of one spelling in one rule share a slot. Occurrences of `_` never
do, because every `_` denotes a distinct variable.

At most 64 distinct variables per rule, per D-7 and architecture section 25,
which is what lets the frame be a plain array and the set of bound slots a
`uint64_t`. Exceeding it is an error naming the rule and pointing at the
occurrence that would have taken slot 65.

### 4.1 Anonymous variables take a slot only where they are illegal

`_` in an argument of a positive relational body literal binds nothing and is
read by nothing, so it is compiled to a **wildcard** argument and takes no
slot. That is not an optimization: a wildcard is what `_` means there, and a
slot for it would be a frame entry nothing ever reads while counting against
the limit of 64. A rule scanning two wide relations with nothing but `_` would
otherwise exceed the limit for no reason a reader could see.

`_` anywhere else -- in the head, in a negated literal, or in a comparison --
is unsafe, because nothing can ever bind it. Such an occurrence **does** take
a slot, so that the one safety computation of section 5 reports it alongside
the named variables and in the same textual order. The rule is always
rejected, so those slots never reach the evaluator.

The result is an invariant worth stating plainly: **in a rule that passes the
check, every slot is bound by some positive relational body literal.** The
evaluator therefore has no unbound-slot case to handle.

---

## 5. Safety is one set computation

Architecture section 14 requires range restriction: every head variable, every
variable of a negated literal and every variable of a comparison must be
bindable from a positive relational body literal.

Let *B* be the set of slots occurring in an argument of a positive relational
body literal, and *R* the set of slots occurring in the head, in a negated
literal or in a comparison. The rule is safe exactly when

```text
R \ B = {}
```

and the offending variables are precisely the members of `R & ~B`. Both sets
are `uint64_t` masks, so the whole analysis is two accumulating ors and one
and-not.

**No reordering search is required**, and this is the reason the check is a
few lines rather than a fixed point over literal orderings. Every positive
relational literal can always be scheduled before every negated literal and
every comparison, because a positive literal needs nothing bound in order to
be scanned. So if a binding order exists at all, that one is such an order,
and asking whether a variable is bindable reduces to asking whether any
positive literal mentions it.

### 5.1 The offenders are reported in the order they were written

Diagnostics are emitted in slot order, each pointing at its variable's first
occurrence. That is textual order, and it is so by construction rather than by
sorting:

> A variable in `R \ B` occurs in no positive relational body literal at all.
> Every one of its occurrences is therefore in a required position, so its
> first occurrence *is* its first required occurrence. Slots are assigned in
> first-occurrence order, so slot order over the offenders is the textual
> order of the occurrences the messages point at.

A rule reports every unsafe variable, not just the first. Each is an
independent mistake, unlike a cascade of parse errors after one bad token, so
suppressing the second would hide a real fact about the rule. Compilation then
continues to the next statement, so one run reports everything wrong with the
program.

---

## 6. Diagnostics

Errors. Each names the variable and the rule, and the column points at the
occurrence, so the message does not also have to say whether it was in the
head, in a negated literal or in a comparison -- the reader can see that.

A name is an identifier of arbitrary length and is shortened to forty bytes
for a message, because one left whole would push the rest of the sentence past
the end of the diagnostic buffer and a message would lose its meaning rather
than a detail. A shortened name ends in `...`, so that a reader knows they are
not looking at the whole spelling and so that advice quoting one -- the
singleton warning's suggested rename is the case that exists -- is visibly
incomplete rather than quietly naming a variable that does not exist.

```text
graph.dl:1:6: error: variable X in fact edge/2 is not bound by a positive body literal
graph.dl:5:18: error: variable Y in rule ready/1 is not bound by a positive body literal
graph.dl:3:5: error: anonymous variable in rule foo/1 cannot be bound by a positive body literal
graph.dl:9:412: error: rule wide/1 uses more than 64 distinct variables
```

A statement with an empty body is called a fact and one with a body is called
a rule, because that is what the writer wrote. The tree represents both as a
rule with a body of some length, per `lib/ast.h`, and only the wording
distinguishes them.

The anonymous case is a separate message because the advice differs. A named
variable can be bound by adding a literal; `_` never can, and telling its
writer to add a literal would be wrong.

Warnings, none of them fatal. The singleton form is the one architecture
section 7 specifies, verbatim:

```text
project.dl:12:14: warning: variable Y occurs only once in this rule
project.dl:12:14: note: rename it to _Y to silence this warning
graph.dl:4:12: warning: predicate blocked/1 is referenced but never defined
```

A variable is a singleton when the rule mentions it exactly once and its
spelling does not begin with `_`. `_Unused` is exempt by that rule and `_` is
exempt because it is never a named variable at all, which is the practice
architecture section 7 adopts so that a rule can document a column it ignores.

**A variable already reported as unsafe gets no singleton warning.** The
error is the message that matters, and advising a rename would be advice to
disguise the bug.

The undefined-predicate warning is reported once per predicate, at the first
place the program mentions it. A predicate that is referenced and never
defined is a typo far more often than an intentionally empty relation, and
the consequence is silent: every positive literal over it fails and every
negated literal over it succeeds.

---

## 7. When the check runs, and what it caches

```c
int relic_check(relic_t *);
```

`relic_check()` validates the loaded program and returns 0 or -1, reporting
every mistake through the diagnostic callback. Evaluation will call it when
the program has not been checked since the last load.

**Compilation is incremental, and that is what makes caching trivial.** The
syntax tree is append-only: a load splices its statements onto the end and
nothing ever rewrites one. The program therefore holds a cursor -- the last
statement a check compiled -- and a check compiles from the one after it to
the end. The cursor is a pointer rather than a count so that resuming costs
nothing proportional to what is already done; statements live in the program
arena and are never freed while the context is, so it cannot dangle.

Three consequences follow, none of which needs a flag of its own:

* a check with nothing new to do compiles nothing and reports nothing, which
  is exactly the caching architecture section 51 asks for;
* each statement is compiled once for the life of the context, so a
  load-check-load-check sequence costs the program once rather than once per
  check;
* each diagnostic is emitted once, rather than repeated by every later check.

What does need to be remembered is whether anything has ever failed, because
an unsafe rule stays in the tree and stays unsafe: a later load cannot repair
it. A sticky flag on the program records that, and `relic_check()` returns -1
while it is set even when the statements it just compiled were all fine.

The undefined-predicate sweep runs at the end of every check, over the whole
table, skipping predicates already reported. It is not incremental because
its answer is not: a predicate referenced by an early load may be defined by a
later one, and the sweep must see the definitions before it speaks. A caller
that checks between loads can still be told about a predicate a later load
goes on to define. That is a warning about a program that was, at that moment,
incomplete, and it is the price of answering a program-wide question without
an end-of-program event that an embedded caller does not have.

### 7.1 Failure has two kinds and every step says which

A mistake in the program and a resource the library could not obtain mean
opposite things to the cursor, so compiling a statement answers with one of
three outcomes and every step that can fail returns it:

* **rejected** -- the statement is wrong and has been reported. The sticky
  flag is set, the cursor advances past it, and compilation continues with the
  next statement. Repeating its messages on a later check would tell the
  reader nothing new.
* **incomplete** -- the library could not finish looking: it ran out of
  memory, or out of predicate identifiers. Nothing was learned about the
  statement, so the cursor does not advance, the sticky flag is not set, and
  the check stops and returns -1. A later check resumes at the same statement.

The reason travels with the failure rather than being inferred afterwards from
a flag. A design that inferred it -- "a step that gave up without setting
`rejected` must have run out of memory" -- rests the cursor's correctness on a
list of which failure site sets which flag, and such a list is only ever as
good as its last entry: one site that neither reports nor marks would make
`relic_check()` refuse a program silently and forever, with the cursor stuck
on the statement it could not classify.

The undefined-predicate sweep is skipped when a check stopped early, because
its answer depends on having seen every definition and some of them are in the
statements that were not reached.

Between compilation and the sweep, the check runs the dependency analysis of
[design-stratify.md](design-stratify.md) section 8, which rejects recursion
through negation and builds the evaluation schedule. It runs before the sweep
so that a check that runs out of memory has answered neither program-wide
question and reports its failure and nothing else; the check that retries it
answers both.

The program arena's failure flag is cleared at the boundary of the check, for
the same reason `lib/parse.c` clears it at the boundary of a load: leaving it
set would make one transient exhaustion refuse every later check for the life
of the context.

---

## 8. The compiled rule

The compiled form exists so that no later stage re-resolves anything. A
planner reading it needs no symbol table and no variable names, and the
executor needs no case for a name that has not been numbered yet.

```c
struct relic_arg {
	union {
		relic_dom_t	constant;
		uint16_t	slot;
	}			u;
	enum relic_arg_kind	kind;	/* CONSTANT, VARIABLE or WILDCARD */
};

struct relic_atom {
	relic_arg_t		*args;	/* the predicate's arity of them */
	relic_location_t	 where;
	relic_pred_id_t		 pred;
};

struct relic_rule {
	relic_arg_t		*head_args;
	relic_literal_t		*body;
	relic_variable_t	*variables;
	size_t			 body_length;
	relic_location_t	 where;
	relic_pred_id_t		 head;
	uint16_t		 variable_count;
};
```

An atom stores no arity. Its predicate has one, and a second copy of a fact is
a second thing to keep in step. A literal is a positive atom, a negated atom
or a comparison, and it reuses the parser's own classification enum rather
than declaring a parallel one for the same three cases.

A variable record keeps the interned spelling, the location of its first
occurrence and how many times the rule mentions it. The first two are what a
diagnostic and later `-e` output need; the third is what the singleton check
reads.

Everything variable-length is allocated from the program arena, so a compiled
rule lives exactly as long as the statement it came from. The array of rules
is a growable array over `reallocarray()` rather than an arena block, because
an arena cannot resize and a rule must be reachable by index once the
dependency graph exists.

### 8.1 A statement is compiled in one pass

The rule is built as it is analysed, because everything whose length the build
needs is known before the walk begins: the head's arity, the body's length and
each atom's arity all come from the syntax tree, and only the variable count is
discovered along the way, into a fixed array of 64.

Safety is still decided afterwards, because it cannot be known until the whole
body has been read. A statement that then turns out to be unsafe is discarded
rather than appended, so nothing incomplete is ever reachable through the
program.

The alternative -- analyse first, build second -- would spend no arena memory
on a rejected statement, and would pay for it with a lookup. The build would
have to find each variable's slot again by name, and that lookup could not fail
for any reason a reader could name, because the analysis has just assigned
every slot it would ask for. An unreachable branch whose unreachability rests
on an argument about an earlier pass is exactly what this project designs out.

What the single pass costs instead is the arena memory of a statement that
turns out to be rejected. It is bounded by the statement, spent once for the
life of the context, and spent only on a program that will not be evaluated
anyway.

---

## 9. Queries are not compiled here

Architecture section 11 turns a query into a rule over a synthetic predicate
whose arguments are the query's output columns. `relic-plan.md` places that
transformation in S5, where the planner that consumes it lives, and it stays
there.

The program model therefore interns the predicates a query's body names, so
the table describes the whole program and the undefined-predicate warning
covers a typo in a query, and does nothing else with it. The query's own head
interns nothing: it names no predicate, and the query is answered into a
relation the query object owns, per `specs/design-query.md`. A query's
variables are numbered and its safety is checked when it is compiled into its
synthetic rule, against
the same rule this document states, because the synthetic rule's head *is* the
list of output columns and "every head variable is bound" is exactly "every
output column is bound".

Until then `relic_check()` does not reject an unsafe query. `STATUS.md` says
so, and `TODO.md` records it.

---

## 10. Limits

| Limit | Value | Where enforced |
| --- | --- | --- |
| arguments in an atom | 32 | `lib/parse.c`, where the argument is written |
| distinct variables in a rule | 64 | `lib/rule.c`, where the slot is assigned |
| distinct predicates | 2^32 - 2 | `lib/pred.c`, checked rather than assumed |

Both structural limits come from D-7 and architecture section 63 and are not
configurable. The arity bound is what lets an index key be a `uint32_t` column
bitmask; the variable bound is what lets a binding frame be a plain array with
a `uint64_t` bound-mask.

Nothing else is bounded. A program may hold any number of rules and a rule any
number of body literals.

---

## 11. Determinism

The program model contributes three things to the reproducibility of a run:

* **predicate identifiers are assigned in first-appearance order**, head
  before body and body in textual order;
* **slots are assigned in first-occurrence order**, head before body;
* **diagnostics are emitted in statement order**, and within a statement in
  the textual order of the occurrences they point at, which section 5.1 shows
  is what slot order already gives.

Nothing iterates a hash table to produce output, and no message derives from a
pointer value.

---

## 12. What is deliberately absent

* **An arity-mismatch diagnostic.** `(name, arity)` is the identity, so there
  is no such mistake to report.
* **A reordering search in the safety check.** Section 5 explains why one
  fixed order settles the question.
* **A declared or inferred type for a column.** Predicate arguments are
  untyped, per architecture section 10.1; a column may hold values of more
  than one type and comparisons across types are settled at evaluation.
* **Removal from the predicate table.** Nothing un-defines a predicate, and a
  table that must delete would need tombstones in the identifier table that
  nothing else in the tree needs.
* **A distinction between extensional and intensional predicates.** It is
  derivable from `defined` and the rules, and the stage that needs it is the
  stage that should define it.
