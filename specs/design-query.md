# relic query object design

**Status.** Design document for the query object: what a prepared query owns,
where its answer is stored, how the answer is named, accounted and released,
and what that leaves in the context afterwards. Forward-looking, like every
document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 11, 17, 20, 49 and 52;
`specs/design-plan.md` section 7, which compiles a query into its rule;
`specs/design-eval.md` section 7, which answers it; `specs/design-relations.md`
sections 2 and 4, which own and discard every other relation;
`specs/design-api.md` sections 5 and 6; `specs/decisions.md` D-4, D-6,
D-12 and D-17.

---

## 1. What this document settles

Five questions, once, so that the compiler, the planner, the evaluator, the
Magic rewrite, the explain output and the relation store's diagnostics answer
none of them again:

* what a query object owns, and what of it survives in the context after the
  query is destroyed -- nothing in the predicate and relation tables;
* where a query's answer lives, and why it is not the relation of a predicate;
* what the head of a query's rule is, when it names no predicate, and how
  every stage learns the head's arity;
* how a relation that belongs to no predicate is named, in `-e` output and in
  diagnostics;
* how the answer is counted against the tuple and memory limits.

It does not cover how a query's text becomes a rule, which is
`specs/design-plan.md` section 7, nor how the rule is answered, which is
`specs/design-eval.md` section 7 and `specs/design-magic.md`. Both are read
here as given; what this document decides is what the rule's head refers to
and where its tuples go.

---

## 2. The answer is the query's

A query compiles into a rule whose head is the list of its output columns,
and answering it runs that rule once into a relation, which is then streamed
to the caller and emptied. The relation exists for exactly as long as the
query object does: it is created with the query, filled and emptied by every
run, and has no meaning once the query is destroyed.

A query that streams fills no relation. When the plan of the rule it runs
derives each of its rows once, and the context sets no tuple or memory
limit, its derivations are its answer already, and its rule delivers them
to the caller as it derives them (`specs/design-comparison-cost.md` section
4). Such a query still owns an answer relation, which stays empty through
that run, and everything below about filling, streaming and emptying the
relation is about the runs that do not stream.

**The predicate table is the wrong place for it.** A predicate is program
vocabulary. The table that interns predicates is context-lifetime state whose
program entries are never removed and whose identifiers are never reused
for another program predicate -- a rewrite's are the run's,
`specs/design-segments.md` section 7 -- and that is
what makes a predicate identifier usable as a dense index by every later
stage -- the relation table, the dependency graph and the rewrite's
per-predicate arrays are all indexed by it (`specs/design-program.md` section
3). Were a query's answer the relation of a predicate, every query prepared
would add a permanent entry for a transient thing, and every stage sized by
the predicate count would pay for each query ever prepared. Six such stages
run once per answer:

* the dependency graph of the rewrite's generated rules, whose vertex count
  is the predicate count;
* the stratification of that graph, whose scratch arrays are sized by it;
* the delta tracking of an evaluation, three arrays over every predicate,
  set up for the complete part and again for the generated rules;
* the discard of every relation before an evaluation, which walks the
  relation table, a prefix of the predicate table;
* the rewrite's complete-predicate analysis, which maps every predicate to
  its component;
* the undefined-predicate sweep, which runs over the whole table when a
  query is prepared and again before it is answered.

A session that prepares, runs and destroys one query per line would then
cost, per line, a walk over every line before it, and a run of queries would
be quadratic. No one of those walks is wrong: each is a legitimate function
of the vocabulary. What would be wrong is a vocabulary that grows with the
queries. Making one walk incremental would leave the other five, and the
next stage that iterates over predicates would join the list. The answer
therefore stays out of the table, so that the predicate count is a function
of the program alone and every present and future per-predicate cost is
bounded by the program, with nothing for a stage to remember.

**So the query owns its answer relation.** A query object holds a
`relic_relation_t` of its own, of the query's output arity, created when the
query is prepared or loaded and released when it is destroyed. No predicate
is interned for it, and no symbol. The predicate and relation tables hold,
after a query is destroyed, exactly what they held before it was prepared,
except for predicates the query's body introduced, which are vocabulary and
belong there (section 4). The claim `relic.h` makes for `relic_query_run()`
-- that answering many queries does not grow the context -- holds of both
tables as well as of the tuples. Symbols and values a query's text introduces
are a separate matter, and section 11 says what is not done about them.

```c
struct relic_query {
	relic_t			*ctx;
	relic_arena_t		 arena;		/* text, syntax tree, rule parts */
	relic_rule_t		 rule;		/* head RELIC_ID_NONE */
	const relic_rule_t	*group_rules;	/* its aggregates' group rules */
	size_t			 group_rule_count;
	relic_relation_t	*answer;	/* the query's, of the output arity */
	bool			 running;
};
```

The query's serial is recorded once, in its answer relation (section 5),
since naming the answer is the only thing the serial is for.

The relation is an allocation of its own rather than a field of the query
struct for the reason `specs/design-relations.md` section 2 gives for every
relation: a plan holds its address across an evaluation, and the relation
layer creates, empties and frees relations as units. Nothing distinguishes an
answer relation from a program relation once a plan has its pointer; the
store's insertion, membership, iteration and limit checks are the same
functions over the same struct. Two things about it differ, and both are
settled below: it is named by its query rather than by a predicate (section
5), and it is not in the relation table (section 3).

**A query holding an aggregate also owns its group rules**, one per
aggregate literal, compiled with the query's rule into the same arena and
never appended to the program's rule table, so preparing such a query
changes no program and discards nothing derived. Each fills a group
relation that a run evaluates after the program's part and before any rule
the rewrite generates, and that is emptied with the answer relation when
the run ends, so outside a run the query holds nothing derived and counts
nothing against the limits. The group predicates are interned, as a
rewrite's generated predicates are, but named by the literal's position
and the group's arity and never by the query's serial, so every query with
an aggregate at the same position and of the same arity shares one entry:
the table grows with the shapes of the queries, not with their number.
`specs/design-aggregates.md` sections 5.2 and 10 give the names and the
sequence.

---

## 3. The answer relation

**Created with the query.** `relic_query_prepare()` creates the relation
after the query's rule has compiled and before the query is published, so a
query that is refused creates nothing and a query that is handed out always
has one. `relic_query_loaded()` creates one for the copy of a program-written
query it hands out. Creation can fail only for memory, and fails the
preparation as any other allocation there does. The arity is the rule's head
arity -- the number of output columns -- fixed when the rule is compiled,
exactly as a program relation's arity is fixed by its predicate. The number
of output columns is bounded by the arity a relation can have,
`RELIC_MAX_ARITY`, because an answer is a relation and each row is one of its
tuples; a query with more named variables is rejected, per
`specs/design-plan.md` section 7.

**Emptied at both ends of every run.** Answering a query empties the answer
relation, runs the query's rule -- the one written, or the one its Magic
rewrite produced -- with the answer relation as the plan's emit target,
streams the relation, and empties it again, whether the answer was delivered,
stopped by the callback or failed part way. A query that streams runs its
rule with the caller's callback as the emit target instead, and its
relation is empty before, during and after the run. The emptying before the
run is what makes the answer's contents right by construction: whatever the
relation held, the run starts from nothing, and no ordering between the run
and any other discard has to be reasoned about. The emptying after the run is
what makes the answer transient, per `specs/design-api.md` section 5.1:
outside a run the relation holds nothing and counts nothing against the
limits.

**Empty whenever another operation could see it.** The relation holds tuples
only between the run of the query's rule and the end of the same
`relic_query_answer()` call, and the context is busy for the whole of that
call (`specs/design-api.md` section 6), so no other operation on the context
runs while an answer relation is non-empty. In particular the discard of
every relation, which runs before an evaluation begins, never runs while an
answer holds tuples, and a query is destroyed only when its answer is empty
or not at all, since destroying a running query is refused. Section 6 makes
the accounting exact without relying on this, and makes it a checked
assertion.

**Released with the query, reading nothing of the context.**
`relic_query_destroy()` frees the relation and the query. An empty relation
owns only its own allocations -- the struct and its array of index headers --
so freeing it touches no context state, which preserves the contract of
`specs/design-api.md` section 6 that a query may be destroyed after its
context. That contract is also what rules out retiring a predicate on
destroy: a destroy that edited the context's tables would read a context
that may be gone (section 11).

**Not in the relation table.** The relation table is indexed by predicate
identifier and is a prefix of the predicate table; an answer relation has no
identifier and no slot. `relic_destroy()` therefore releases no answer
relations, because it never owned any, and a context holds no pointer into a
query object except while that query is being answered: the EMIT operation
of the plan holds the relation's address, in the evaluation arena, which is
released before the answer returns.

**The relation layer's interface.** Three operations serve both the table's
relations and a query's, so that no caller reaches into a relation's storage:

```c
relic_relation_t *relic_relation_create(relic_t *, relic_pred_id_t pred,
    size_t serial, uint16_t arity);
void relic_relation_clear(relic_t *, relic_relation_t *);
void relic_relation_free(relic_relation_t *);
```

A predicate's relation is created with a serial of zero, which nothing
reads: the serial names a relation only when `pred` is `RELIC_ID_NONE`.

`create` allocates an empty relation and records what it is called (section
5) and its arity; the table creates a predicate's relation through it, and a
query its answer. `clear` empties one relation and gives back what it counted
against both totals; it is the one way a relation loses tuples, used by the
answer at both ends of a run and by the discard of every relation for each
relation in the table. `free` releases a relation that `clear` has emptied and
touches nothing else, which is what lets a query be destroyed after its
context.

---

## 4. The query rule's head names no predicate

The rule a query compiles to keeps its shape: a head made of the output
columns, and the body as written. Its `head` is `RELIC_ID_NONE`, since there
is no predicate. What the stages downstream read from a head is settled here,
so that none of them looks a query's head up in the predicate table, where
it would find nothing.

**Every rule carries its head arity.** A rule's head arity is the number of
its head arguments, fixed when the rule is compiled:

```c
struct relic_rule {
	...
	relic_pred_id_t		 head;		/* RELIC_ID_NONE for a query */
	uint16_t		 head_arity;	/* head arguments */
	...
};
```

Every stage that needs a head's arity reads the field -- the rewrite's
renumbering of a rule into canonical form, the comparison and hash that
recognize a duplicate generated rule, the explain output -- for a query's
rule and a program's alike. A rule is built in three places, and each sets
the field where it sets the head: the compiler of a statement's head, from
the interned predicate; the compiler of a query's head, from the count of
output columns; and the rewrite's canonical form, from its draft, which
carries the head arity beside the head -- a source rule's for an adorned
rule, the number of bound columns for a magic rule, the query's for the
rewritten query. The field is not a copy that can drift from the predicate's
arity, because arity is half of a predicate's identity and nothing can change
it once the head is interned; what could go wrong is a constructor that
leaves the field unset, and the planner catches that, below, for every rule
whose head is a predicate. A query's answer relation is sized from the
query rule's head arity, so for a query's rule the planner's comparison is
of the field with itself; what guards it is that the compiler sets the head
arity where it builds the head's arguments, from the same count.

**The plan emits into a relation its caller names, and confirms it fits.**
The planner takes the relation the EMIT writes from its caller rather than
resolving it from the head's predicate: the evaluator hands a component
member the relation of its head predicate, and hands a query's rule the
query's answer relation. The planner confirms that the relation's arity is
the rule's head arity and reports an internal error otherwise, in the same
way it confirms a delta literal is a positive literal of the rule. The
confirmation is what makes a head arity an array bound the plan can trust:
the EMIT reads exactly `head_arity` head arguments into a tuple of the
relation's arity, and a rule constructed with the wrong arity fails loudly at
planning rather than reading past its arguments.

**Nothing outside the evaluator's query path reads a query's head.** The
result's width is the answer relation's arity; the rows streamed are the
answer relation's tuples; the explain output names the answer through the
relation (section 5). The evaluator's query path -- which plans and runs a
query rule on its own, apart from the components of a schedule -- is the one
place that names the answer relation, and it never indexes anything by the
head: a query's rule is a component of one rule with no delta variants, so
the member positions the components of a schedule are indexed by are not
consulted for it.

**The dependency graph never sees a query rule.** A query's rule is not in
the program's rule table, and the rewritten query is kept apart from the
generated rules, per `specs/design-magic.md` section 6; the graph is built
over rule tables only, so no head without a predicate can reach it.
`specs/design-stratify.md` states the consequence: a query is not part of the
graph.

**The rewrite treats the query's head as a call with nothing bound.** The
query's head is the source and the adorned predicate of a pseudo-adornment
with no bound column and no magic predicate. Its guard is absent, its bound
slots are the head's arguments under an empty mask, and a limit reached while
generating a magic rule is attributed to the predicate being called, never to
the rule whose body made the call, so nothing about the pseudo-adornment
consults the predicate table. The rewritten query rule copies the query's
head and head arity and runs into the same answer relation. What
`specs/design-magic.md` section 6 says of the query holds.

---

## 5. Names

**A relation knows what it is called.** A relation that belongs to a
predicate is called by the predicate's name; a relation that belongs to a
query is called `$q` and the query's serial, and records the serial:

```c
struct relic_relation {
	...
	relic_pred_id_t		 pred;		/* RELIC_ID_NONE for an answer */
	size_t			 serial;	/* the query's, when pred is none */
	uint16_t		 arity;
};
```

One function, `relic_relation_name()`, renders the name, and
`relic_relation_describe()` adds the arity to it where a diagnostic wants
`name/arity`; they are the only way a relation is named: every `-e` line
that names an operation's relation, and every diagnostic of the relation
store -- a limit reached while deriving into or
indexing a relation, a relation too large to grow, an internal error over an
index or a statistic -- goes through it. The `-e` output and the diagnostics
are therefore the same text whether the relation is a predicate's or an
answer:

```text
emit $q4(X)
relic: error: limit exceeded: tuples=1000 while deriving $q4/1
```

The explain entry points that print a query's rule receive the query's
answer relation as well as the rule -- the query's own, or the rewritten one
-- and print the head through the same function, so the head of a query rule
and the EMIT of its plan are named by one mechanism. Program rules and
generated rules have heads that are predicates and are printed through the
predicate table as they always are.

**The serial.** It is the serial of `specs/design-plan.md` section 7.1: it
counts the queries the context has compiled, in compilation order, and a
rejected query takes none. The program's queries are compiled by
`relic_check()` in load order and prepared queries as they are published,
and the two interleave -- a load, a prepared query, another load -- so a
loaded query's serial is not its index. The table of the program's queries
therefore records a serial beside each compiled rule:

```c
struct relic_loaded_query {
	relic_rule_t	rule;
	size_t		serial;
};
```

and `relic_query_loaded()` hands out a query under the number the check gave
it, with an answer relation that records that number.

**`$q<serial>` is a display name.** It is built from the serial when it is
printed and is neither a symbol nor a predicate in the context's tables. It
cannot be mistaken for anything a program writes, because `$` cannot appear
in an identifier the lexer accepts -- D-12, whose purpose is that a reader of
`-e` output can tell a generated name from a user's, and which a name that
exists only in that output serves as well as an interned one would.
Diagnostics that the compiler issues about the query's text call it "the
query", per `specs/design-plan.md` section 7, and the tool numbers queries
for its own messages, per `specs/design-cli.md` section 2.2; `$q<serial>` is
the name of the query's relation, wherever a relation is named.

Determinism is D-4's: identical sequences of loads and preparations number
their queries identically, because the serial is a function of the caller's
own operations.

---

## 6. Accounting

An answer's tuples count against `max_tuples` and `max_memory` exactly as a
program relation's do: the relation store's insertion checks the limits and
adjusts the context's totals for every relation it inserts into, and it does
not ask whether the relation is in the table. A query whose answer would
exceed a limit fails before a row is delivered, as `specs/design-eval.md`
section 7 requires, and the failed answer is emptied and its counts given
back. A query streams only in a context that sets neither limit, so every
answer a limit could bound is gathered in its relation and bounded there.

**The totals are a sum, maintained per relation.** `relic_relation_clear()`
subtracts what the relation it empties held, and the discard of every
relation is `clear` applied to each relation in the table. The totals are
then the sum of what every relation holds, in the table or not, by
arithmetic: no operation sets a total to a value it did not compute from a
relation. Zeroing the totals after the walk would be exact only when every
relation is in the table, which the answer is not, or when every answer is
empty at the moment of the discard, which section 3 argues but which is an
ordering rather than a property of the numbers.

**The argument of section 3 is an assertion.** When assertions are enabled,
the discard of every relation confirms afterwards that both totals are zero.
That is true exactly when no relation outside the table holds anything,
which is what section 3 promises of every answer relation, so a future path
that let an answer stay non-empty into a discard fails a test rather than
skewing the totals.

**`relic_stats()` counts the answer being delivered.** It reports the
context's totals, which include the answer's tuples and bytes until the
answer is emptied, so a row callback that asks sees the answer counted and a
caller outside a query sees none, which is what `specs/design-api.md`
section 5.1 promises.

---

## 7. Tests

* **Growth.** A unit test prepares, runs and destroys a query many times
  against a small program and confirms that the predicate count, the
  relation table's size and the tuple and storage totals of `relic_stats()`
  are what they were after the first query. The cumulative counters of
  `relic_stats()` -- iterations, derivations, tuples inserted, scanned and
  looked up -- grow for the life of the context by design and are not
  compared.
* **Naming.** The test that the program's queries are `$q0`, `$q1` in load
  order and a prepared query takes the next number, and that a query between
  loads takes the number between, reads each name from the `-e` output.
* **Explain and diagnostics.** The golden `-e` outputs and the limit
  diagnostics of `tests/lang` are unchanged, which checks that a relation
  named by its query is described exactly as one named by a predicate.
* **Width.** `relic_query_collect()` on a query answering nothing reports the
  query's number of columns, read from the answer relation.
* **Lifetime.** Under AddressSanitizer, a query is destroyed after its
  context; a query answered to a limit failure is destroyed with nothing
  leaked and the totals at zero; a query whose preparation fails at the
  answer relation's allocation leaks nothing.
* **Session cost.** `tests/perf.sh` runs an interactive session of many
  query lines, on the clock with the cycle workload's budget as that workload
  is, since no counter measures a walk over the predicate table; linear, it
  takes about a second, and quadratic, minutes.

---

## 8. Determinism

Nothing here orders anything by a pointer, a hash or an identifier. The
serial is assigned in the order of the caller's operations; the answer
relation iterates in insertion order as every relation does; the `-e` output
is a function of the program and the queries. Two runs over the same input
produce the same rows in the same order and the same explain text.

---

## 9. What is deliberately absent

* **Retiring the answer predicate on destroy.** A destroy may run after the
  context is gone, per `specs/design-api.md` section 6, so it cannot edit the
  context's tables; and the identifier table behind symbols, values and
  predicates has no removal operation, so retirement would mean adding
  tombstones to a structure three other subsystems depend on, to serve one
  transient entry.
* **One shared answer predicate per arity.** Bounding the growth by sharing
  `$q/N` among every query of arity *N* would make two live queries the same
  predicate, and would make the correctness of their answers rest on answers
  being transient and the context non-reentrant. Both are true; neither is a
  property a predicate should be relying on.
* **One answer relation owned by the context.** A scratch relation given
  each query's arity and name at the start of its answer would spare the
  query object its relation and its allocation at prepare. It would also be a
  relation whose arity and name change between answers and whose contents
  mean something only while one particular query is running -- the shared
  predicate's objection in a weaker form -- and it would give two prepared
  queries a piece of shared state where they have none. A relation's arity is
  fixed for its life everywhere else in the store, and stays so here.
* **Discarding only the relations an answer touched.** It would make one of
  the six walks of section 2 incremental and leave the rest, and the cost it
  removed would return with the next stage that iterates over predicates.
* **Releasing the Magic rewrite's generated predicates here.** They are
  the run's, appended above the program's count and released with the
  rewrite, per `specs/design-segments.md` section 7; this design neither
  depends on that nor is changed by it.
* **Releasing the symbols and values a query's text introduces here.** A
  query's variable spellings and source name never enter the pool, and its
  constants are values in a template that each run instantiates above the
  pool's mark and releases with the run's derived relations, per
  `specs/design-segments.md` sections 4 and 5. A collected result owns the
  bytes it holds, section 8 there, which is what lets the identifiers of
  an answer be the run's.
* **Keeping an answer across runs.** An answer is emptied when delivered,
  per `specs/design-api.md` section 5.1; a query object that owns its
  relation could keep it, but a cached answer that a later load or fact
  silently invalidates is worse than one recomputed.
