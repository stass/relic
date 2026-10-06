# relic table segments design

**Status.** Design document for what a context's tables keep and what a run
gives back: the symbol pool and the value tables gain a boundary between
what the program wrote and what one run introduced, everything above the
boundary is released when the run's derived relations are, and the
predicate table releases a rewrite's predicates with the rewrite.
Forward-looking, like every document under `specs/`; `TODO.md` records
what is not yet implemented.

**Companions.** `specs/architecture.md` sections 17, 18, 19, 47, 49, 52 and
63; `specs/design-memory.md` sections 2, 6, 7 and 8, whose tables this
design segments; `specs/design-program.md` section 3, whose predicate table
it changes; `specs/design-magic.md` sections 4 and 11, whose generated
predicates it makes the run's; `specs/design-query.md` section 9 and
`specs/design-strings.md` section 9.1, which left this problem open;
`specs/design-facts.md` sections 4 and 5.3, whose staging and discard it
extends; `specs/design-eval.md` sections 6 and 7, whose run paths it gives
one prologue; `specs/design-api.md` sections 3, 5, 6 and 9;
`specs/decisions.md` D-3, D-4, D-6, D-12, D-17, D-24 and D-34.

---

## 1. What this document settles

Five questions, once, so that the lexer, the compiler, the rewrite, the
evaluator, the relation store and the public interface answer none of them
again:

* which entries of the context's tables belong to the program and which to
  a run, where the boundary between them lies, and which single function
  moves it;
* what the symbol pool holds, and where a name that is not a value lives
  instead;
* when a query's constants become identifiers, and how long they stay ones;
* how a table gives back its run entries without a tombstone, a free list or
  a reference count;
* what may leave the context holding an identifier or a pointer, and under
  which contract.

It does not change what a value is, how a tuple stores one, or how the
Magic rewrite derives its rules. Those are read here as given from
`specs/design-memory.md` section 8, `specs/design-relations.md` and
`specs/design-magic.md`; what this document decides is the lifetime of the
entries they intern.

---

## 2. Where the growth comes from

A context holds four interning tables -- the symbol pool, the integer table,
the float table of `specs/design-floats.md`, and the predicate table -- and
none of them releases an entry. That is right for the program: its facts,
rules and names are what the caller wrote, and `specs/design-memory.md`
section 2 bounds them by the text. It is wrong for three kinds of entry that
a run introduces and that nothing holds once the run is over:

* **Names that are not values.** The lexer interns every identifier and
  every string literal it reads, so a variable's spelling and a predicate's
  name enter the pool beside the values, and the parser interns the source
  name so that a location stored in the tree stays printable. A session
  therefore grows the pool by one entry per line, because the tool names
  each line's query `<query N>`, and a program that spells its variables
  differently from the last one grows it by its variables.
* **A query's constants.** `path(a, X)?` interns `a` as a value when the
  query is prepared. An embedder preparing a query per request over distinct
  keys grows the pool or the integer table by an entry per distinct key, and
  nothing ever reads those entries again.
* **A rewrite's vocabulary.** Every adorned, magic and supplementary
  predicate and every base view enters the predicate table, with a symbol
  naming it, and stays. The rewrite is built again for every answer and
  keeps no cache, so the entries serve nothing between answers; and every
  array an evaluation sizes by the predicate count -- the dependency graph,
  the strata, the deltas, the membership marks -- is sized by them from then
  on, which is the cost D-17 took the query's answer relation out of the
  table to avoid.

The values a run computes -- `cat` interns a string per derivation,
arithmetic an integer -- are a fourth kind, already bounded by the memory
limit and already useless after the discard that frees the relations
holding them. They are released by the same mechanism, for free.

The common shape is a lifetime problem rather than a table problem: one
append-only table holds entries of two lifetimes, the program's and a run's,
and a table that cannot tell them apart cannot release either. The design
tells them apart by position.

---

## 3. Two segments, one boundary

The symbol pool and the value tables are each divided at a **mark** into a
**program segment** below it and a **run segment** above it. The program
segment holds what loads, imports, fact additions, the check and the
caller's own interning put there, and it grows for the life of the
context. The run segment holds what a run introduces:
the identifiers a query's constants bind to and the values evaluation
computes. It is released whole, by rewinding the table to the mark. The
predicate table is not divided at the mark; section 7 gives it a boundary
of its own, at the run's granularity, for a reason given there.

**The invariant.** *A segment lives exactly as long as something the
context owns holds an identifier from it.* The context owns two kinds of
holder: the program -- its base stores, its compiled rules, its predicate
entries -- which holds program identifiers only; and the store's derived
relations, which hold identifiers from the run segment of the run that
wrote them. Nothing else in the context holds an identifier across an
operation, which section 8 makes true for the public interface as well.

Two mechanisms keep the invariant, and each is one function rather than a
rule every caller remembers.

**One interning function, and the run decides the segment.** Every
interning of a value -- a fact's values as a load or an import stages them,
a fact the caller adds, a rule's constants as the check compiles it, the
caller's `relic_value_symbol()` and `relic_value_string()`, a query's
constants as a run instantiates it, and the values a batch or a round
commits -- goes through one function, and which segment it appends to is
decided by the context's state rather than by the caller: whether a run is
**open**. The prologue opens the run when it takes the mark and the
epilogue closes it. While no run is open the function is the **program
funnel**: it looks the value up **at or below the mark**, an identifier
found above the mark being a run's and so a miss; on a hit it returns the
program's identifier and changes nothing; on a miss it **discards the run
state** (below), appends the value, which the rewind has just made the
table's tail, and moves the mark past it. While a run is open the function
appends above the run's mark and never discards; the batch commit of
pending values asserts a run is open, and the funnel's discard asserts
none is. No caller chooses, so a program value cannot land in the run
segment, nor a run value in the program segment, through a call site that
picked the wrong function; and the operations that intern while no run is
open -- loads, imports, fact additions, the check, which runs inside the
program's readiness before any mark is taken, and the caller's interning,
which an operation in progress refuses -- are the program's by that fact
alone, with no list to keep.

The discipline "a program operation appends only while the run segment is
empty" is therefore not a property of loads, imports, fact additions and
the check, which this design does not ask to discard, but of the one
function they intern through: an append cannot happen above a run segment
because the append is the step after the discard. A load of facts the
program already holds, a load whose every value the program has, and the
caller's interning of a value the program has all find their values below
the mark and discard nothing, so `include/relic.h`'s promise that adding a
fact the program holds changes nothing, and `specs/design-facts.md`
section 5.4's that a load of duplicates leaves the relations current, both
stand. A load that adds a tuple discards at its commit, for the relations'
sake; the funnel may have discarded earlier, while the parser staged the
first new value, and the second discard finds nothing to do. A load,
import or fact addition that stages a new value and is then refused has
discarded for a program it did not change, which costs the next run a
materialization and nothing else.

**The run prologue.** Every run -- a query answered through
`relic_query_run()` or `relic_query_collect()`, a materialization -- begins
with one prologue, in this order: the program is made ready, which runs the
check; **the run discards if it will derive into the store**, which is
exactly when the relations are not current (`specs/design-facts.md`
section 5.4), since a run over current relations answers from them and
every other run computes a fixed point or a rewrite; the run **takes its
mark**, the tables' counts at that moment; and the run **instantiates its
query** (section 5), which interns the query's constants above the mark.
No other point in a run discards: the discard that precedes a rewrite's
derivation and the one that precedes a materialization are this one, and
a run that abandons its rewrite over budget and answers from the whole
program re-enters the prologue -- discards, marks, instantiates again --
rather than discarding in the middle of a derivation. An instantiation
made before a discard is released by it, and the one made after is what
the derivation reads; nothing reads a constant across a discard, by
construction, because the only discard inside a run is followed by an
instantiation.

**The run epilogue.** A run that discarded in its prologue **leaves its
segment**: its relations hold identifiers from it, and the next discard,
from any run's prologue or the program funnel, releases both. A run that
did not discard **pops its segment** when it ends, successfully or not:
it answered from current relations and leaves the store as it found it.
Such a run writes two things that hold its identifiers, the group
relations its aggregates derive into the store and the answer relation
the query owns, and both are emptied before the pop: the epilogue is the
last step of the run, after the group run is finished and the answer
delivered and cleared, as `specs/design-api.md` section 5.1 already has
them emptied. Which of the two a run does is decided in its prologue and
recorded in the run, not inferred at its end from the store's state. Every run that discards does so before it takes its mark,
so at most one segment of a discarding run exists at a time, directly
above the program mark, and a run that does not discard stacks its own
above that and pops it on the way out. The stack is never deeper than two,
by construction rather than by a count.

**The discard** is the one function of `specs/design-facts.md` section
5.3, extended: it frees the derived relations, drops the base stores'
secondary indexes, clears the materialization record, and **rewinds the
pool, the value tables and their accounting to the program mark, whether
or not it found a derived relation to free**. The rewind costs nothing
when the count is already the mark, and making it unconditional is what
makes orphans impossible: a run that discarded, derived nothing into the
store and interned a value in between leaves an entry above the mark with
no relation holding it, and the next discard, which finds no relation to
free, still returns the entry.

**What is not reclaimed.** The program segment. A long-lived context that
loads program after program keeps every program's vocabulary, as it keeps
every program's rules; that is what the caller wrote, and
`specs/design-memory.md` section 2 leaves it outside every limit for that
reason. A context whose program is replaced wholesale is a context the
caller destroys and creates again. The predicate table's program entries
are in the same case, and a query's atoms add to them: a prepare interns
every predicate a query names, per `specs/design-program.md` section 9,
so that the undefined-predicate warning covers a typo in a query, and the
group predicates a query's aggregates generate are shared by position and
arity. A stream of queries naming distinct predicates the program does not
define grows the table by those names, bounded by `RELIC_PRED_MAX`; that
is a property of the program model's decision to intern a query's atoms,
which this design keeps, and the interface documentation states it as a
limit on untrusted query text.

---

## 4. The pool holds values and nothing else

The symbol pool is the one path every byte string that is a *value* takes,
per `specs/design-strings.md` section 9.1, and it stops being the path for
anything else. **The lexer and the parser intern nothing.** An identifier
token carries its bytes and length in the text being parsed, a string
token its decoded bytes in the lexer's buffer, and the parser keeps what it
needs of them in the statement's arena:

| What the text names | Where it lives |
| --- | --- |
| a symbol, string, integer or float constant | the statement's arena, as a value, until section 5 binds it |
| a variable's spelling | the statement's arena; the compiled rule's variable records the bytes and length for diagnostics and explain |
| a predicate's name | the context's name store (section 7) |
| a function's or an aggregate's name, `not` and `_` | nowhere; the lexer and the parser recognize them by comparing bytes |
| the source name | the arena of what was parsed under it: the program arena for a load, the query's arena for a prepared query, the import's staging for an import |

A statement's arena is the program arena for a loaded statement, which
lives as long as the context does since statements are never freed
(`specs/design-program.md` section 7), and the query's arena for a prepared
one, released with the query. A variable is resolved within its statement
by comparing bytes: a rule has at most 64 variables (architecture section
63), so the comparison is a short scan and needs no table. A fact's values
are staged as identifiers, as `specs/design-facts.md` section 4.1 has
them, obtained through the program funnel of section 3.

**Source names and the location contract.** A location's `file` is a
pointer into the arena of what was parsed under the name, so it lives as
long as that does: for the life of the context when a load produced it, of
the query when a prepared query did, of the call when an import did. The
interface documentation says so in place of its promise that every `file`
lives as long as the context, and the diagnostic callback's rule -- a
callback that keeps a location copies it -- then has no exception for the
name. The tool copies what it prints and keeps nothing.

**The one location the context retains** past its statement is the
predicate table's record of the first atom reading a predicate,
`referenced_at`, which the undefined-predicate sweep of
`specs/design-program.md` section 6 points at. A query's atom can be that
first reference, and the query's arena dies with the query, so the function
that records the reference copies the location's source name into the
context's name store, deduplicated against the names already there, once
per predicate since only the first reference is kept. It is the only place
a location crosses from a statement into the context, so it is the only
place that copies; a second such place would copy as well, and a
maintainer adding one finds the rule at the function every retention goes
through.

The rewrite's generated variable names, `X1`, `X2` and so on, are bytes in
the rewrite's arena like the names of any statement's variables, and the
generated predicates of section 7 have no symbol at all.

What this removes is every path by which text that is not a value reached
the pool: the pool's count is then the number of distinct byte strings the
context has interned as values, and a session's lines, a program's
variable names and a query's source name leave nothing behind. Architecture section 18's "all symbols and string literals
should be interned" stands, read as the values it was about; its "the
evaluator should almost never compare C strings" stands too, since the
parser is not the evaluator.

---

## 5. A run instantiates its query

A query is compiled once and answered many times, and it is answered after
discards that it has nothing to do with. Its constants therefore cannot be
identifiers in the run segment at compile time, and section 2 says they
must not be identifiers in the program segment either. They are neither:
**the compiled query is a template whose constants are values**, bytes and
length or an integer or a float in the query's arena, wherever a constant
can occur -- an atom's argument, an operand of a comparison or an
expression, an argument of a call, and the same places in the group rules
its aggregates generate and in each aggregate's own body. The template is
a type of its own, `relic_template_t`, holding the rule and the group
rules with their constants as values, and the one function that takes it
is the instantiation: the planner, the rewrite, the stratifier, explain
and the executor take a `relic_rule_t`, so a call site that reached for
the template where a rule is read fails to compile rather than reading a
value's bytes as an identifier.

**Every run instantiates the template**: the prologue copies the query's
rule and its group rules into the **run arena** -- an arena of the run's,
initialized in the prologue and reset in the epilogue, uncharged like the
query arena since its size is the query's -- and interns each constant as
it copies it, in the order the text wrote them, through the interning
function of section 3 with the run open, storing the identifier in the
copy's argument. The copy is deep: a literal, an expression and an
aggregate are copied as the rewrite's copy of each already does, given the
run arena to copy into, and an aggregate's `body` in the copy points at
the copy's own group rule rather than at the template's, so that the body
the rewrite hashes and compares to find duplicate rules is the instance
holding identifiers. The instance is what the planner, the rewrite and the
executor read; none of them knows when an identifier was assigned. A
loaded query and a prepared one are instantiated alike, because the
template is the same object by either route (`specs/design-plan.md`
section 7.2), and a loaded query handed out several times by
`relic_query_loaded()` shares its template with every copy, which is safe
because a template is never written.

A run that abandons its rewrite over budget finishes the group run the
rewrite began, resets the run arena, and re-enters the prologue, which
discards, takes the mark again and instantiates again; nothing of the
first instance is read afterwards, since the group run that held it is
finished and the arena that held it is empty.

Instantiating rather than overwriting the compiled rule is what makes the
constants' lifetime a consequence of the arena's rather than of a list of
fields to overwrite: an aggregate's body, a group rule the query owns
through a `const` pointer, and a template shared by two query objects are
each copied, so no shared structure is mutated and nothing stale outlives
the run arena.

A constant the program has -- `a` in `path(a, X)?` when some fact names `a`
-- interns to the program's identifier, by the one-identifier-per-value
property of `specs/design-memory.md` section 8, and the run segment gains
nothing for it. A constant the program does not have gets a new
identifier, which is exactly as long-lived as the magic seed, the
comparison or the computed value that will use it.

An instantiation that fails -- a pool at its limit, memory refused -- fails
the run before anything is derived, with the diagnostic the interning
gives, and the run ends as any failed run does: popping its segment if it
did not discard, leaving it to the next discard if it did.

A program rule's constants are interned when the rule is compiled by the
check, while no run is open and so into the program segment, and never
rebound: they are the program's vocabulary, bounded by its text.

---

## 6. The sequence table and the rewind

Each table rewinds to a mark it took earlier, and the marks of all of them
are taken and rewound together, as one record the context keeps for the
program segment and one each run keeps for its own.

**The symbol pool.** The mark is the entry count and an arena mark of the
string arena (`relic_arena_mark()`, which the pool already takes before it
copies a reservation's strings). Rewinding sets the count back, rewinds the
arena, which frees the chunks allocated past the mark and keeps the ones
before it whole, and unplaces the identifiers above the mark from the
table's index.

**The integer table and the float table.** The mark is the count; the
rewind sets it back and unplaces as the pool does. An integer's bytes are
in the entry, so there is no arena.

**The value table's use bits.** The two bits per symbol identifier that
record whether it was interned as a symbol value and as a string value
(`specs/design-memory.md` section 8) are cleared above the pool's mark with
the entries. A bit a run sets on a program entry -- a computed `"foo"`
whose bytes are the program symbol `foo` -- stays set: the entry is the
program's and is not reclaimed, the bit costs nothing, and the only reader
that can tell is `relic_stats()`, whose `string_values` then counts a
string the context once held. Clearing it would need a record of which
bits a run set, a list this design has no other use for.

**The sequence table.** `specs/design-memory.md` section 6 keeps the
identifier table free of tombstones and says a table that must delete
should be a different type. The pool, the integer table and the float
table index themselves through that different type, the **sequence
table**: an identifier table whose owner assigns identifiers `1..count` in
placement order, which grows in identifier order and rewinds. It wraps
`relic_idtab_t`, so the probe, the placement, the tags and the
three-quarters load are the identifier table's and written once; what it
adds is the growth order and the rewind, and what it withholds is the
slot-order growth. The relations' striped indexes, whose identifiers are
not dense within a stripe, stay ordinary identifier tables and cannot be
rewound, by type rather than by care.

```c
struct relic_seqtab {
	relic_idtab_t	index;
	size_t		count;		/* identifiers 1..count are placed */
};

int	relic_seqtab_reserve(relic_seqtab_t *, size_t added,
	    relic_rehash_fn, void *ctx);
void	relic_seqtab_place(relic_seqtab_t *, uint32_t hash);
void	relic_seqtab_rewind(relic_seqtab_t *, size_t count,
	    relic_rehash_fn, void *ctx);
```

Linear probing admits a rewind under one condition. Call an entry's
*chain* the slots its probe visits from its home slot to the slot it
occupies. A search for an entry fails if any slot of its chain is empty,
so a rewind may empty only slots that lie on no surviving entry's chain.
The condition is:

> *Every slot on an entry's chain holds an entry with a smaller
> identifier.*

It holds at placement whenever entries are placed in identifier order,
because the slots an entry passes were occupied before it was placed, by
entries placed earlier, which have smaller identifiers; and the three
owners place in identifier order, since each entry's identifier is the
count at its placement, a batch's identifiers are promised in order and
placed in order (`specs/design-memory.md` section 8), and nothing places
out of order. It is preserved by a growth that re-places entries in
identifier order, walking `1..count` and asking the owner for each hash
through the rehash function the owner already supplies; a growth that
walks the old slots re-places in slot order, which can put a larger
identifier on a smaller one's chain, and a wrap-around chain makes the
difference observable. `relic_seqtab_reserve()` therefore walks
identifiers, and the identifier table's own reserve, which walks slots,
stays as it is for the tables that never rewind.

Under the condition, `relic_seqtab_rewind()` unplaces the identifiers
above the new count **in decreasing order**: it probes from each
identifier's hash, which the owner supplies as it does for a growth, finds
its slot, and zeroes the slot and its tag. The probe finds the identifier
because every slot on its chain holds a smaller identifier, each either at
or below the new count or not yet unplaced; and no surviving entry's chain
loses a slot, because every slot on a surviving entry's chain holds an
identifier smaller than a survivor's, which is a survivor. A probe for an
absent key still terminates, since the rewind only adds empty slots. The
cost is one probe per unplaced entry, so a discard costs what the run
interned and nothing proportional to the program. The tag is zeroed with
the slot so that no empty slot carries a stale tag; the probe tests
emptiness before it tests the tag, so a stale tag would be harmless, and
the zeroing keeps the two from ever being asked to agree.

Rebuilding the table from the surviving entries instead would be simpler
to state and would cost the program's size at every discard, which a
session issuing a query per line would pay per line; it is left absent as
the rewind, and kept as the shrink below, where it is paid only after a
run that was itself at least that large.

**A rewind bounds, and shrinks what one run outgrew.** The entry arrays,
the use-bit array and the slot array keep the capacity the largest run
reached, since the next run will fill them again and
`specs/design-comparison-cost.md` section 8 leaves what a growth leaves
behind to the allocator; what the rewind returns to the system at once is
the string arena's chunks past the mark. A capacity is charged to
`max_memory` whether or not it is used, so a capacity one large run
reached would hold a share of the limit for the life of the context and
refuse a later load for a run that is over. Every rewind, a discard's and
a pop's alike, therefore **shrinks a table whose capacity exceeds four
times what the count it rewinds to needs**: the entry array and the
use-bit array are reallocated to the capacity that count's growth would
have reached, and the sequence table is rebuilt at the smallest capacity
holding that count at the identifier table's load, re-placing in
identifier order. The rebuild costs the surviving entries, and it follows
only a run that interned at least three times them, so the cost is
amortized against that run. A shrink is best effort: a rewind cannot
fail, so a reallocation or a rebuild the allocator refuses leaves the
larger table, which is correct and merely charged as before, and the next
rewind tries again. No probe spans a rewind, as none spans a growth. A
shrink is a function of the counts alone, so what a table holds after a
rewind, and the charge it carries, is the same at every thread count and
in every run of one input.

**Accounting.** The pool and the value tables are charged to `max_memory`
whole (`specs/design-strings.md` section 9.1); a rewind recomputes the
charge from what remains, as the discard already recomputes the store's,
so after a discard the account is the base stores, their canonical indexes
and the program segment's tables at the capacities the shrink left them,
and `storage_bytes` says so.

---

## 7. The rewrite's predicates are the run's

The predicate table is **not** segmented by the mark of section 3: a
prepared query appends a predicate it names while a run segment may exist,
and a predicate entry is permanent, so predicate entries cannot sit above a
boundary that a discard rewinds. The table instead has a boundary of its
own, at run granularity, where the stack discipline holds by construction:
a run may append generated predicates above the program's count, nothing
else may append while a run is in progress, since the context is busy, and
the run releases them before it ends.

**The run's predicates.** A rewrite creates each adorned predicate, magic
predicate, supplementary predicate and base view by a walk whose order
`specs/design-magic.md` section 12 fixes, and nothing outside the rewrite
looks one up by name: no atom of a program or a query can spell a name
holding `$` (D-12). So the rewrite **appends** them, above the program's
count, without placing them in the table's index, and keys nothing on
their names. Where the rewrite itself asks twice for one predicate -- the
base view of a source it reaches from two places, the supplementary
predicate a prefix names again -- it finds the first answer in a **map of
its own**, in its arena: an array from program predicate to base view,
sized by the program's count, and a table from (adorned predicate, rule,
call, arity) to supplementary predicate, keyed as the adornment set is
keyed by (source, bound columns), the arity included because it was part
of the interned name's key. The maps say what the interning said by
accident,
and live as long as the rewrite does.

An entry records what a generated predicate is instead of what it is
called:

```c
struct relic_pred {
	...
	relic_pred_id_t		stands_for;	/* the program predicate it is part of */
	uint32_t		bound;		/* adornment: column c bound when bit c is set */
	uint32_t		rule;		/* supplementary: its rule among those deriving the adorned predicate */
	uint32_t		call;		/* supplementary: the call it precedes, from 1 */
	enum relic_pred_kind	kind;		/* program, adorned, magic, supplementary, base view, group */
};
```

`stands_for` is the program predicate for every kind, and `RELIC_ID_NONE`
for what is generated from the query's own rule, as the rewritten kind
records it, so that `relic_pred_describe()` goes on describing a generated
predicate as the program's predicate it is part of or as the query;
`bound` is
the adornment of an adorned predicate, of its magic predicate and of the
adorned predicate a supplementary one belongs to; `rule` and `call` are
the two ordinals of `specs/design-magic.md` section 4. The **name is
rendered** from the entry when it is printed, by the one function that
names a relation (`specs/design-query.md` section 5) and by explain:
`path$bf`, `magic$path$bf`, `sup$path$bf$1$2`, `path$base`, and
`sup$$q0$1$2` for the query's own rule, byte for byte the names of
`specs/design-magic.md` section 4, as `$q<serial>` is rendered from a
serial. A name that exists only in output needs no symbol, and the pool
gains nothing per rewrite.

**Released with the rewrite.** `specs/design-magic.md` section 11 releases
the rewrite's rules and arenas when the query has run; its predicates and
their relations go with them, in the same reset, which runs before a
rewrite abandoned over budget falls back to the whole program. The reset
frees, through the relation store, every relation of every predicate
above the program's count -- derived relations, and a base-store slot a
read covered for a generated predicate that has none -- and truncates
every per-predicate table of the store and the predicate table itself to
that count, so that the next rewrite's predicates take the same
identifiers from the same base and find no slot of an earlier one. The derived relations that survive a rewrite's
answer are those of the program predicates it read completely, and they
are freed, with the run's value segment, by the next discard.

Between runs the predicate table therefore holds the program's predicates,
those the program's and the queries' aggregates generate and those queries
have named, and every per-answer walk is sized by that: the rewrite's
share of the growth, which D-17's bound did not foresee, is returned, and
the queries' share is the limit section 3 states. The budget on generated
predicates, `max_magic_preds`, counts per rewrite.

**The program's predicates.** Their names leave the pool with the other
names of section 4. The context gets a **name store**, an arena of
alignment one like the pool's string arena, never rewound and charged to
`max_memory` as the pool is, holding each program predicate's name and the
source names of section 4's retained locations; an entry keeps a pointer, a length and the hash it was placed
by, exactly as a pool entry does. The table's key is the name's bytes and
the arity: the hash is the bytes' hash under the context's seed with the
arity mixed in as one more word, and the probe compares the arity and
then the bytes, which a predicate interning pays once per statement rather
than once per tuple, at the call site as every identifier-table comparison
is. A group predicate's name is rendered into a buffer and interned as
bytes. `relic_pred_t` loses its `name` identifier; D-24's "a symbol is
interned once" stands for values, and a name that is also spelled as a
value is stored twice, a predicate's worth of bytes.

---

## 8. What leaves the context

Section 3's invariant needs nothing outside the context to hold a run
identifier, or a pointer to a run's bytes, past the run, and nothing to
bring one back in. Four rules make that so.

**A result owns its bytes.** `relic_query_collect()` copies rows out of the
context, and `include/relic.h` promises that a result "holds nothing of the
context's state" and may outlive the context; it keeps the promise only
because every identifier is permanent. A result that held a computed
string's identifier across the discard that reclaimed it would read, later,
whatever string the next run placed there, silently. So **a result interns
the symbols and strings it holds into a pool of its own** as it collects
them, copying each distinct byte string once, and its values' identifiers
are the result's. A result's identifier has **bit 31 set**, which no
identifier of a context has, so that every function of the context --
`relic_string()`, `relic_value_compare()`, `relic_fact_add()` -- refuses a
result's value as one it never assigned instead of reading some unrelated
value of its own: a caller that passes a result's value where a context's
is expected gets NULL or a diagnostic, never a wrong string. The result
has its own accessors:

```c
const char	*relic_result_string(const relic_result_t *, uint32_t id,
		    size_t *lenp);
int		 relic_result_compare(const relic_result_t *,
		    const relic_value_t *, const relic_value_t *);
```

`relic_result_string()` reads the bytes as `relic_string()` reads the
context's, and refuses as never assigned an identifier without bit 31 or
one past the result's count; `relic_result_compare()` orders two of the
result's values in the total order of `specs/design-memory.md` section 8,
which the tool's sort needs since the tool collects an answer it sorts,
and treats a value not the result's as the context's compare treats one
not the context's, below. A caller that feeds a result's value back as a
fact builds it from the bytes with `relic_value_symbol()` or
`relic_value_string()`; the interface documentation's remark that a value
read out of an answer may be inserted changes to say so. A result then
truly holds nothing of the context: it may be read after the context is
destroyed, which the promise allowed only while the context lived, and no
identifier of the context's is in it. Its integers are already values.
The collector's pool is the symbol pool's type, owned by the result,
hashing under the context's seed so that an answer cannot be crafted to
crowd it, and charged to nothing, since a result is the caller's. The
change reaches the tool's formatting and sort of collected rows, the
shipped example, which reads a collected value with the context's
accessor and keeps a streamed value past its callback, and the interface
documentation, and it changes what an existing function's value means, so
the library's version-info moves as `specs/design-api.md` section 9 has
it move for an interface change.

**A value entering the program is the program's.** Every interface that
takes a `relic_value_t` into the context -- `relic_fact_add()` and
whatever joins it -- checks the value where it already checks that an
identifier was assigned at all, and refuses with a diagnostic a symbol or
string identifier above the program mark: such an identifier is a run's,
which no caller holds legitimately, and a base store that stored it would
hold, after the next discard, an identifier the next run assigns to
another value. The check is in the one function that admits a value, so
no entry point has to remember it.

**A row callback's values and bytes are the context's while the callback
runs.** A streamed row names its values by the context's identifiers, and
the bytes `relic_string()` returns for a value a run introduced live in
the string arena's chunks that the rewind frees, so a caller that keeps a
value or a pointer past the callback keeps the bytes instead. The
interface documentation's promise that a row's bytes stay valid until the
context is destroyed becomes a promise for the duration of the call, the
same the row itself already has. The tool formats each row inside the
callback and keeps nothing; an embedder that wants rows later collects
them.

**`relic_string()` outside an operation refuses an identifier above the
mark.** The context knows whether an operation is in progress, since every
operation enters it (`specs/design-api.md` section 6). Inside one, every
assigned identifier is readable, which is what a callback needs. Outside
one, the run segment belongs to no caller: a run that popped has returned
its identifiers, and a run that left its segment will have it rewound by
a discard the caller does not see coming. So `relic_string()` called
outside an operation returns NULL for an identifier above the program
mark, as it does for one the context never assigned, and a caller holding
one from a callback finds out at once rather than reading another run's
value later. `relic_value_compare()`, which cannot return a failure,
reports a diagnostic for a value the context does not hold -- outside an
operation, one above the mark; anywhere, one it never assigned -- and
returns 0, so that an embedder sorting with a value that is not the
context's hears about it instead of getting a quietly unsorted answer;
it is the one read-only accessor that can report. The bytes of a program
value stay valid until the context is destroyed, as promised, since the
program segment is never rewound.

**Interning a value the program does not have is a change to the
program.** `relic_value_symbol()` and `relic_value_string()` promise an
identifier that stays valid for the life of the context, which only the
program segment can give, and they go through the program funnel of
section 3: a value the program has is returned from below the mark, and
one it does not have is interned the way a fact's value is, after a
discard. A caller that interns a key before adding the facts that use it
pays one discard, which the fact addition would have paid; a caller that
interns a new value between answers of a materialized program pays the
materialization, and the interface documentation says so, with
`relic_value_compare()` and a result's bytes as the ways to compare that
cost nothing.

**A location's `file`** lives as long as the arena of what was parsed under
it, section 4; a diagnostic callback that keeps a location copies it,
name included.

The rules above leave no supported way to hold a run's identifier or a
run's bytes across the operation that made them, or to bring one back
into the program, and make the unsupported ways loud wherever the context
can tell: outside an operation, and at every interface that admits a
value. Inside a later operation's callback a stale identifier names
whatever value holds it then, which is what keeping a row past its
callback forfeits.

---

## 9. Limits

Nothing new is configurable. `RELIC_PRED_MAX` bounds the predicate table
with the run's generated predicates included; the pool's and the value
tables' ceilings of `specs/design-memory.md` sections 7 and 8 bound the
program segment and the run segment together, since the identifiers are
one space; and `max_memory` charges the tables and the name store whole,
so a run that interns more than the limit admits is refused at the
interning, and the refusal ends the run as section 5 ends a failed one. A
result's pool has the symbol pool's ceiling and no charge.

---

## 10. Tests

* **Unit, the sequence table.** A table grown in identifier order
  satisfies the chain condition: after every growth and every rewind, every
  surviving identifier is found by its probe, and a probe for an absent key
  terminates; a wrap-around chain -- keys whose home slots sit at the end
  of the slot array -- is among the cases. A rewind to a count, then
  placements to the former count, then a second rewind, finds the same
  survivors. A tagged and an untagged table both. A rebuild at a smaller
  capacity finds every survivor.
* **Unit, the pool and the value tables.** Intern, mark, intern, rewind:
  the count, the arena's chunks, the use bits above the mark and the charge
  return to the mark's; an identifier above it decodes to nothing; the same
  bytes interned again get the same identifier they had. A run that
  interned four times the program segment leaves a shrunk table after the
  discard, with the charge of the program segment's capacity.
* **A session's lines.** A thousand `<query N>` queries against one program
  leave `relic_stats()`'s `symbol_values`, `string_values` and
  `storage_bytes` where the first left them, in the REPL test and through
  the library.
* **A query per request.** Prepare, run and destroy a query over a
  distinct constant a thousand times, against a program whose facts do not
  hold the constants; the stats are flat. The same against a materialized
  program, with and without an aggregate in the query: flat, and the
  program stays materialized.
* **The funnel.** Materialize a program whose rule computes `"foo"`, then
  load `p("foo").`: the fact's value is a program identifier, the
  materialization is discarded, and the next answer is right. Materialize,
  then load a rule whose constant the program does not have, then answer:
  the rule's constant is a program identifier. Load a fact the program
  holds: nothing is discarded.
* **The prologue.** A query whose rewrite goes over budget and is answered
  from the whole program gets the right answer over a constant the program
  does not have; a query that computes a value over a program with no
  rules leaves nothing above the mark after the next discard.
* **A rewrite's vocabulary.** A thousand answers of one query through the
  rewrite leave the predicate count at the program's; a thousand distinct
  adornments, through queries binding different columns of a wide
  predicate, do too. A rewrite that reaches one source's base view from
  two places generates one view.
* **Names.** `-e` output of every golden test is byte-identical: rendered
  names equal interned ones. Diagnostics naming a variable, a predicate, a
  source name or a query's source name are byte-identical, the
  undefined-predicate warning for a predicate first referenced by a query
  that has since been destroyed among them.
* **Results.** A collected result read after a further query, a load, a
  fact addition and the context's destruction gives the same bytes and the
  same order under `relic_result_compare()`; a result holding a computed
  string survives the discard that reclaimed the string; a result's value
  passed to `relic_string()`, `relic_value_compare()` and
  `relic_fact_add()` is refused.
* **Stale identifiers.** An identifier a row callback received for a
  computed value is refused by `relic_string()` after the run returns.
* **Interning by the caller.** Interning a value the program has leaves a
  materialization current; interning one it does not have discards it and
  the identifier survives every later run.
* **The differential suite** (architecture section 66) is unchanged and
  passes at every strategy and thread count: instantiating at run time and
  releasing at discard change no answer.
* **Sanitizers.** `make check-asan` and `make check-ubsan` over the suite:
  a rewind that freed a chunk still referenced, or unplaced an identifier
  still held, fails here first.

---

## 11. Order of work

Four steps, each shippable alone with its tests, in the order that closes
the most growth first:

1. **The pool holds values only** (section 4, and section 7's name store):
   names, variable spellings and source names leave the pool; the retained
   location copies; the location contract. This alone closes the session's
   growth per line.
2. **Results own their bytes, and the callback contract** (section 8):
   `relic_result_string()`, `relic_result_compare()`, the tool's sort, the
   example, the interface documentation and the version-info. Independent
   of the tables and a prerequisite of step 3.
3. **Segments, the funnel, the prologue and the rewind** (sections 3, 5
   and 6): the sequence table, the marks, the program funnel, the run
   prologue and epilogue, the discard that rewinds and shrinks, the
   instantiation, and `relic_string()` outside an operation. Closes the
   growth per constant and per computed value.
4. **The rewrite's predicates are the run's** (section 7): the rewrite's
   maps, rendered names, appended entries, released with the rewrite.
   Closes the growth per adornment and restores D-17's bound on the
   per-answer walks.

---

## 12. Determinism

* A run's identifiers depend on the program, on the segment below it and
  on the run alone: a run that discards starts at the program mark, so two
  such runs of one query assign the same identifiers whatever ran between
  them; a run over current relations starts above the materialization's
  segment, which is a function of the program. Without this design an
  identifier depends on every query the context has ever seen; this is
  stricter, and D-4 stands.
* Rendered names are a function of the entry and equal the interned names
  they replace, so `-e` output and diagnostics are byte-identical.
* The rewind's order, the growth's order and the shrink's threshold depend
  on identifiers and counts, never on hashes or slots, so the seed of
  `specs/design-hash-seed.md` decides nothing it did not decide before.
* `relic_stats()` after a discard reports the program segment's tables,
  with the use bits a run set on program entries, the same at every thread
  count since the parallel merge interns the same values in the same order
  (`specs/design-strings.md` section 9.2).

---

## 13. Amendments this design makes

Applied with this design, in the documents it amends:

* `specs/design-memory.md` section 2: the string arena's row of the
  lifetimes table, the run arena and the name store, and a discard that
  rewinds; section 6: "No deletion" names the sequence table as the
  different type; sections 7 and 8: the mark, the rewind, the use bits'
  rule, and the life of a `relic_string()` pointer.
* `specs/design-arithmetic.md` section 8: the integer table is rewound
  with the relations.
* `specs/design-comparison-cost.md` section 7.2: the life of a
  `relic_string()` pointer.
* `specs/design-program.md` section 3: the entry's fields, the name store,
  the key, the retained location's copy, and generated predicates appended
  above the program's count and released with the run; section 4: a
  variable's spelling is bytes.
* `specs/design-magic.md` section 4: names are rendered, predicates are the
  run's, the rewrite's maps; section 11: predicates and relations are
  released with the rewrite.
* `specs/design-query.md` section 9: the two absences on releasing the
  rewrite's predicates and a query's symbols and values are resolved here.
* `specs/design-strings.md` section 9.1: the pool is rewound to the mark at
  a discard; the absence on reclaiming interned strings in section 19 is
  resolved here.
* `specs/design-plan.md` section 7: a compiled query is a template whose
  constants are values, instantiated at each run.
* `specs/design-facts.md` section 4.1: a fact's values are interned through
  the program funnel; section 4.4 and 5.3: a discard with nothing derived
  rewinds the tables, and the discards are the commit's, the prologue's
  and the funnel's.
* `specs/design-eval.md` section 6: the run prologue is the one place a
  run discards; section 7: the query rule a run reads is the instance.
* `specs/design-query.md` section 2: a rewrite's identifiers are the run's.
* `specs/design-api.md` section 3: the interning rule, `relic_string()`
  and `relic_value_compare()` outside an operation, and a value entering
  the program; section 4: a discard with nothing derived rewinds; section
  5: the callback's contract and the result's own pool and accessors;
  section 6: a location's `file`; section 9: the version-info.
* `specs/architecture.md` sections 17, 18, 19 and 47: a predicate's name
  is in the name store, the pool holds values, a public identifier's life,
  and the arena table gains the run arena and the name store.
* `specs/decisions.md`: D-34 records the decision and says what it amends
  in D-12 and D-17; D-24 stands for values. Decisions are not rewritten.

Applied with the implementation, since they describe code: the contracts
in `include/relic.h` for the value's identifier, `relic_string()`, the row
callback, the diagnostic callback's location, `relic_value_symbol()`,
`relic_value_string()`, `relic_value_compare()`, the result, the sort the
header suggests for collected rows, and `relic_fact_add()`; the tool's
formatting and sort of collected rows in `cmd/relic/output.c`;
`examples/embed.c`; and the manual pages that repeat them. The
version-info moves once for the step that ships the first of these
contract changes after the interface is frozen, and again for each later
step that ships separately; before the interface is frozen it does not
move.

---

## 14. What is deliberately absent

* **Tombstones, a free list or a reference count in any table.** A rewind
  is a stack operation on a table whose placement order is its identifier
  order; it needs none of them, and `specs/design-memory.md` section 11
  keeps them absent.
* **A per-query scope.** The scope here is the run's, not the query's: a
  query is a template whose constants are values, and holds no identifier
  between runs, so there is nothing of a query's to scope.
* **Segmenting the predicate table at the discard's mark.** A prepare
  appends to it while a run segment exists, and a predicate is permanent;
  the run-granularity boundary of section 7 is the one the table admits.
* **A query atom over an unknown predicate reading an empty relation
  without an entry.** It would stop a stream of queries naming distinct
  unknown predicates from growing the predicate table, and it would give
  the compiled rule, the dependency graph and the stratifier an atom that
  names no predicate, a case each would have to carry. Section 3 states
  the growth as a limit instead; the program model's decision to intern a
  query's atoms stands.
* **Asking loads, imports and fact additions to discard before they
  intern.** It would make every load discard, which the interface promises
  a load of what the program holds does not; the program funnel discards
  exactly when something new is appended, even when the operation that
  appended it then fails.
* **Owning a query's group relations by the run.** A run over current
  relations would then write nothing into the store, and "the run
  derives into the store" would equal "the relations are not current" by
  construction rather than by the two emptyings section 3 orders before
  the pop. The group relations are the store's because a generated rule
  may hold a copy of the aggregate literal and reads its group through the
  store, `specs/design-aggregates.md` section 10; moving them is a change
  to that design, worth making if a third thing a from-current run writes
  ever appears.
* **Rewinding a failed load.** A rejected statement stays in the tree and
  references its predicates, and the constants it interned are bounded by
  its text.
* **Reclaiming the program segment across reloads.** What the caller wrote
  is the caller's to bound; a context is cheap to destroy and create.
* **Rebuilding the sequence table at every discard.** Section 6: it costs
  the program at every discard; the shrink pays it only after a run that
  was at least that large.
* **A result that references the context.** Section 8: it would hold run
  identifiers across the discard that reclaims them, and misread silently.
* **A context-lifetime store for every source name.** It would keep the
  location contract as it is and would grow by one name per session line,
  which is the growth this design closes; a location lives with what was
  parsed under its name.
