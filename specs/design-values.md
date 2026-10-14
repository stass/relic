# Values

How a value becomes the 32-bit domain identifier a tuple column holds, and
how an identifier becomes a value again. Every value, integers included, is
interned: the fact path and the program check intern the constants a program
writes, evaluation interns the integers and strings it computes, and from
then on joins, hashing and duplicate elimination are integer work over dense
arrays that never inspect a payload. Only comparison, sorting and output
decode an identifier. Values are a foundation under every pipeline stage.
This document covers the common identifier table, the symbol pool,
the value table, the interning of computed values, and the hash function
with its per-context seed.

Related: [architecture: values](architecture.md#values),
[architecture: determinism](architecture.md#determinism),
[language: values and types](language.md#values-and-types),
[memory](design-memory.md), [storage](design-storage.md),
[front end](design-frontend.md), [expressions](design-expressions.md),
[parallel evaluation](design-parallel.md), [library interface](design-api.md).

## Model

A **value** is a symbol, a string or an integer (a float type is designed in
[floating point](design-floats.md)). Inside the engine it is a **domain
identifier**: 32 bits whose top two give the type and whose bottom thirty
index the table of that type.

* Symbols and strings share the **symbol pool**, which gives each distinct
  byte string one symbol identifier. The symbol `foo` is that identifier under
  the symbol type bits and the string `"foo"` the same identifier under the
  string type bits: one copy of the bytes, two distinct values.
* An integer is its position in the **integer table**, an array of 64-bit
  integers with an identifier table over it.
* The **value table** owns the integer table and, per symbol identifier, two
  **use bits**: whether those bytes have been interned as a symbol value, and
  whether as a string value. They make a symbol or string identifier one the
  context actually assigned, and they count distinct values by type.

Interning happens at three points only. A fact's values are interned as the
input is accepted for staging by a load, an import or a fact addition;
a rule's or query's constants when the program check compiles the statement;
a value evaluation
computes when the batch holding it is resolved, before it reaches a relation.
A statement that fails before it is compiled or staged interns nothing.
A fourth path puts bytes in the pool without making them a value: a caller
building a value through `relic_value_symbol()` or `relic_value_string()`
interns the bytes, admitted and charged like any other, and leaves the use
bits alone until the value enters the program.

An import can prepare a bounded window's values in private dictionaries.
It assigns new pool and integer identifiers in global first-field-occurrence
order and publishes only after a whole-window proof and reservation.
Symbol and string descriptors share byte equality but request separate
use bits. An unproved window follows scalar replay before changing values,
preserving the scalar failure prefix. The batch adapter shares the ordinary
value-layer equality and growth functions; no stripe bits enter the domain
identifier. See [bulk import](design-bulk-load.md#decoding-and-value-dictionaries).

## Invariants

* **One encoding per value.** There is one path to a domain identifier: a
  symbol's identifier is assigned once, by the pool, and its value's
  identifier is a function of it; an integer's position is assigned once, by
  the integer table. Equal identifiers mean equal values, so the store
  compares words.
* **Zero is never an identifier.** Symbol identifiers and integer positions
  start at one and type bits of zero are never assigned, so a zeroed slot,
  column or frame entry holds no value, with no occupancy bitmap and no
  sentinel a caller could collide with.
* **Nothing orders values by domain identifier.** Within a type identifiers
  follow first appearance, so their order records the order the input
  mentioned things in; across types it is the order of the type bits. Every
  ordering decodes and compares values.
* **No table is walked in slot order.** Every table answers which identifier,
  if any, has this key; iteration is by identifier. This is why the hash seed
  can be random without changing anything a run reports.
* **The pool holds values, and never names**: the bytes of values and of
  values a caller built through the value constructors. **Nothing is
  un-interned**: the tables grow for the life of the context, bounded by the
  memory limit.

## The identifier table

Interning, duplicate elimination and every index reduce to one question: given
a key, which 32-bit identifier has been assigned to it? The common
open-addressing identifier table answers it without owning the key
(`lib/util.h`). A canonical membership alternative is confined to the
relation payload, as [membership locality](design-membership-locality.md)
describes; it does not widen this table for every owner.

**A slot holds an identifier and nothing else** -- no key, pointer or hash.
The key lives where its owner already keeps it (a symbol's bytes, a tuple's
columns), so an entry costs four bytes and nothing is stored twice; that is
the reason for a purpose-built table rather than a generic container.
A full hash beside every identifier doubles slot storage. A successful
lookup still reads the matching key, so avoided mismatches alone do not
establish that this storage pays for itself. Canonical membership also
measures growth, where a cached hash can avoid rereading tuple columns.

**A tag is the cheaper form of that idea.** A table created tagged keeps one
byte per slot: the top eight bits of the hash the slot's identifier was placed
under. The low bits choose the slot, so the tag separates keys that share a
neighbourhood, and a probe passes an occupied slot whose tag differs without
asking the owner. With well-distributed independent slot and tag bits,
an unsuccessful search compares against one mismatching key in 256 on
average, for a quarter of the slot array's size. Above `2^24` slots those
bits overlap and filtering weakens without affecting correctness. Each
owner chooses once, at creation:

| Table | Its searches | Tagged |
| --- | --- | --- |
| symbol pool, integer table | unsuccessful when new; key far away | yes |
| index keys, staging sets, distinct counts | mostly unsuccessful | yes |
| a relation's canonical index, reference layout | successful duplicate checks are important | no |
| predicate table, rewrite tables | small | no |

The canonical reference stays untagged to avoid an unconditional byte
per slot on the largest table every relation has. Its layout comparison
includes this existing tagged table before introducing another probe
implementation. Selection uses measured hits, misses, growth and total
memory, rather than assuming every stage of evaluation has the same
duplicate rate. The [adoption rule](design-membership-locality.md#adoption-rule)
requires complete workload gains and keeps the reference when additional
metadata does not earn its cost.

**The probe sequence exists in one place.** Slots are probed linearly from the
hash's low bits. A probe cursor hands each candidate identifier to the caller,
which decides whether the candidate's key is the one wanted: the comparison
stays where the reader sees what makes two keys equal, and the probing
arithmetic is never written twice.

**Growth** doubles the table before it passes three quarters full, which
keeps probes short and guarantees an empty slot, so every probe terminates.
Rehashing needs each identifier's hash, which only the owner can compute, so
the owner supplies a callback, called once per entry per doubling and never
on a lookup; an owner that stores a hash (the pool) answers without reading
its keys. Capacity is a function of the count alone.

**Reserving and placing are separate operations, and the order matters more
than it looks.** A reservation makes room and is the only half that can fail;
a place stores an identifier and cannot. An owner reserves *before* it creates
the thing being indexed. The other order looks harmless: store first, index
second. But then a failed growth leaves an entry in the owner's array that no
lookup can reach, and for an interning table that is a broken invariant, not
a lost entry -- the next intern of the same key does not find it, stores a
second entry with a second identifier, and from then on two occurrences of one
symbol compare unequal. Nothing reports it; queries return wrong tuples for
the rest of the context's life because of one transient failure that was
correctly reported at the time. Splitting the operation makes that state
unrepresentable.

A reservation for *n* entries grows at most once, straight to the smallest
power of two that holds them, which is where *n* single reservations would
have led. A reservation can also be split into an allocation that may fail
and a move of the entries that cannot, for an owner (a round's merge) that
allocates while it may decline and moves afterwards, possibly on another
thread. A table can also live in memory its owner gives it, for an owner that
fills many tables once, sized by counts known in advance, and charges their
slots as one block; such a table never grows.

**No deletion, so no tombstones.** Nothing removes an interned value or a
stored tuple. Tombstones are what make linear probing delicate; a table that
must delete should be a different type.

**Striped tables.** A table several threads fill at once without a lock -- a
relation's canonical index, a secondary index's key table, the symbol pool's
index -- is a constant number of ordinary tables, and a key lives in the
stripe chosen by the top bits of a wide hash. The wide hash's low half is the ordinary keyed hash,
which chooses slot and tag; its high half is an *unkeyed* fold of the same
words, computed in the same loop, which chooses the stripe. Unkeyed on
purpose: each stripe grows as its own keys need, so what a striped table holds
depends on how keys fall among stripes, and a keyed choice would make the
storage a program reports, and the insertion a memory limit refuses, differ
between runs. Unkeyed routing lets an author crowd keys into one stripe on
purpose, which costs the table its parallelism and nothing else: slot and
tag stay keyed within the stripe, so crowding builds no probe cluster, and
every capacity projection holds however the keys fall, all in one stripe
included. The stripe count is a constant rather than the thread count for
the same reason.

**Final size and peak are different questions.** A table's growth can be
prepared, an allocation that moves nothing, and completed later by the
part that fills the stripe, so that stripes grow on several threads at
once; until it completes, the stripe's old table is held beside its
replacement. Admission asks the final size, which is what interning or
inserting one key at a time reaches, so a batch is admitted exactly when
its keys one at a time would be. Whether the growth is deferred is
decided after, once per operation -- a window's publication, a load's
commit, a round's merge -- on the calling thread, before the operation's
first reservation, by a pure test that reports nothing and sets no
refusal state: every deferrable growth of the operation defers when its
parts run on a pool and its whole admitted footprint, every table and
destination it reserves, plus every table those growths would replace
fits the limit, and none does otherwise. Parts that all run on the
calling thread gain nothing from deferral, which would only hold every
replaced table at once. A growth that does not defer completes at once,
stripe by stripe, each old table freed as its replacement is filled, and its
overlap stays an uncharged transient, as every growth's old generation
is. From reservation until the parts end the account counts each
receipt's prepared tables beside the tables they replace. A failed or
declined operation releases, with the receipt, the charge for the
prepared tables it frees; the installed tables it leaves, and the
capacities they keep, remain charged. The final
capacities are the same either way, and placement still runs a stripe to
a part. A round's merge admits its relations' growth on the peak instead
and always defers it, since a merge that does not fit declines to the
serial evaluator; its footprint for the test is that admitted peak.

## Domain identifiers

**One uniform 32-bit domain.** A tuple is a dense array of domain identifiers,
four bytes per column whatever the type. The two alternatives fail the same
way, by making correctness rest on every construction site:

* a **tagged 64-bit cell** with small integers inline must box the rest, so
  one integer has two encodings and the parser, the fact path and arithmetic
  must all pick the canonical one; a miss makes `X = Y` silently false for
  equal integers. It also doubles tuple memory in symbol-valued columns,
  where the data mostly is;
* a **plain 16-byte value** puts a type, padding and a union in every column,
  so hashing and comparing need every padding byte zeroed at every
  construction site, at four times the memory.

The representation's one real cost is a column of many distinct integers,
which pays an entry and a tagged slot each. It sits behind a handful of
accessors -- intern, look up, order, hash and compare tuples, format -- so it
could be replaced without touching the planner, the evaluator or the public
interface.

**The type lives in the identifier.** A symbol value is its symbol identifier
under the symbol bits, a string value the same identifier under the string
bits, an integer its position under the integer bits. A symbol needs no
value-table entry: interning one is one probe into the pool and a use bit,
whose array may have to grow and can be refused by the limit like any
growth. A float type takes the integer bits with the index's top bit set,
halving the integers' index space and moving nothing else.

## The symbol pool

The pool (`lib/symbol.c`) maps a byte string to a symbol identifier through a
tagged identifier table over its entries. An entry records a pointer to the
bytes, their length, and the hash it was placed under, which is what the
rehash callback returns and what the tag is cut from.

* **The length is explicit**, because a string may contain NUL bytes; a
  terminator is stored after it anyway, for callers that want a C string. A
  string too long for the length field is refused before a byte is read.
* **The bytes live in a string arena of alignment one**, packed end to end.
  Arena blocks never move, so the pointer `relic_string()` returns stays valid
  until the context is destroyed however often the entry array moves.
* **A new symbol is a reservation of one**, as a batch of computed strings is
  a reservation of many: the memory limit admits the pool's projected size,
  the table and the entries grow (to the stepwise capacity, so the pool holds
  the same bytes however its strings arrived), the bytes are copied after an
  arena mark, and only then is the symbol numbered and placed. A refused copy
  rewinds to the mark, so a failed or declined reservation leaves no copy
  behind, and clears the arena's failure flag -- the one exception to the
  sticky-flag contract
  ([the sticky failure flag](design-memory.md#the-sticky-failure-flag)).
* **The pool is charged to `max_memory` whole**, as are the integer table and
  the use bits.

## Names are not values

The lexer and parser intern nothing but a fact's values. A token carries its
bytes in the text, or for a string in the lexer's buffer, and the parser keeps
what it needs in the statement's arena. A variable's spelling lives there and
is resolved within its statement by comparing bytes -- a rule has at most 64
variables, so a short scan needs no table. Function and aggregate names, `not`
and `_` are recognized by comparing bytes and stored nowhere. So the pool's
count is the number of distinct byte strings interned as values or built by
a caller through the value constructors, and a session's lines or a
program's variable names leave nothing in it.

**Predicate names live in the name store**, with the source name of the one
location the context retains. The store is owned by the predicate table and
charged to `max_memory` with it; both are described in
[the front end](design-frontend.md#what-is-interned-and-where-names-live).
A name also spelled as a value is stored twice.

## Decoding and comparison

**Decoding fills a struct the caller provides**; no public value is stored,
so there is nothing to point at. Decoding refuses an identifier the context
never assigned -- type bits of zero, a symbol whose use bit for that type is
clear, a position past the integer table -- and the public functions that
take an identifier report it rather than read some unrelated value.

**No host bytes enter or leave.** A caller fills the fields the interface
documents and leaves the rest holding whatever its stack held. Interning keeps
only what the type selects -- a symbol's use bit, or the integer -- and
decoding zeroes the caller's struct, padding and the unused half of the union
included, before writing the type and its member. A decoded value has one
definite representation, and the library neither retains nor returns host
memory it was never given, whether or not the caller zeroed its locals. This
is a security property.

**Comparison reads fields, never bytes.** The public value has a union and
padding, so comparing or hashing its representation would read bytes no
assignment defines. Nothing does: an integer is hashed as an integer, a
symbol's bytes by the pool, and tuples store identifiers, not values.

**Two orders, not one.**

* The **language order**, used by the comparison operators, is defined only
  within a type: numeric for integers, bytewise for symbols and for strings.
  A mixed-type pair is *unordered*, which the evaluator turns into a
  filtered-out binding rather than an error, because a column may hold more
  than one type ([language: comparisons](language.md#comparisons)).
* The **total order**, used for sorted output, ranks integer, then symbol,
  then string, and falls back to the language order within a type. It makes
  output stable and is invisible to the language.

Both are bytewise, never locale collation, so no result depends on the
environment. A collected result keeps its bytes in a pool of its own under
identifiers no context assigns, so a context refuses a result's value instead
of misreading it ([library interface](design-api.md)).

## Interning computed values

An integer or string an expression computes has no identifier while a plan
runs, because a plan does not write the context. The compiler enforces it,
serial and parallel alike: the executor is compiled seeing the context only
through `lib/frozen.h`, where it is an incomplete type, so an expression that
interned would not compile. The executor
looks a computed value up read-only -- the lookup half of interning, answering
the assigned identifier or zero. A value without one is written into the
batch's head tuple as a **pending marker**: its position in the batch's
pending array, counting from one, under type bits of zero. No value has zero
type bits, so nothing that decodes can mistake a marker for an identifier,
and only assigned identifiers reach published relations, their indexes
and callbacks. Batches and candidate buffers carry markers privately until
their value-resolution boundary. One pending array holds both kinds -- an
integer, or a byte range in the batch's pending bytes -- so resolution is
one loop over the columns.

**A new value's identifier is known before it is assigned.** The tables
number new entries in order, so a new value's identifier is a function of the
table's count and of the order new values are met. Resolution uses this to
separate what can fail and changes the context from rewriting the batch, in
three steps that mirror an insertion:

1. **Measure** walks the pending values in order and fills a map with each
   one's identifier: the one assigned; for a string whose bytes the pool holds
   without a string value (a symbol's spelling, say), the pool's identifier
   for those bytes, to be made a string value by its use bit; otherwise the
   count plus its rank among the distinct new values of its kind, by first
   occurrence. It returns the bytes the integer table, the pool and the use
   bits hold once the new values are in, at the **stepwise** capacity
   interning one at a time would reach
   ([growable arrays](design-memory.md#growable-arrays)), and writes
   nothing but the map and the caller's probe. The use bits are projected
   from the highest identifier promised, since a string over an existing
   high symbol identifier can need the bits grown and no pool entry.
   The **map** holds what commit needs: each value's identifier and the
   new values in identifier order, with their hashes by value and their
   bytes borrowed from the batch until reservation copies them or cleanup
   ends. The **probe** -- each value's hash and the slots of the table
   that finds a value's first occurrence -- is scratch measurement
   borrows from its caller: the evaluator reuses one across batches, and
   a round's merge releases its allocation and charge once measurement
   ends. A batch import finds its values with its window's own
   dictionaries and numbers them into the map directly, so it has no
   probe.
2. **Rewrite** replaces each marker with its promised identifier and writes
   nothing but the batch, which can then be hashed, screened and classified
   like any other while the tables hold exactly what they held.
3. **Commit** reserves room in the integer table, the pool and the use bits
   at once -- the one step that can fail, leaving their contents as they were
   -- and then appends each new value under exactly the identifier promised
   and sets the use bit of every string promised, new or not, which cannot
   fail. The use bits are reserved up front with the pool, to cover the
   highest identifier the map holds, precisely so that setting a bit at
   append time cannot need a growth. A caller that declines after a
   successful reservation unreserves, releasing the pool's receipt. The
   new symbols are placed in the pool's keyed lookup index on the
   calling thread, in their assigned order.

**Serially**, the evaluator measures, asks the memory limit and refuses as an
insertion refuses, naming the predicate being derived into, and otherwise
rewrites, commits and inserts. Nothing is interned from a batch the memory
limit refuses. A batch the tuple limit refuses at insertion has had its
values committed, since commit precedes insertion; that is the reference
behaviour, and costs a few entries the next evaluation would intern anyway.

**A shared round runs the three steps at three points of its merge**, so
nothing changes the tables until the merge cannot fail. A shared round is a
deferred round, one whose insertions wait for its end, whose work is divided
among threads; it reads the **frozen store**, the store as it stood when the
round began ([parallel evaluation](design-parallel.md#model)). During the
round, a candidate with a pending column is new against the frozen store --
a value the
context lacked when the round began is in no relation the round reads -- and
is screened against the worker's own candidates by value, a pending column
hashing and comparing by its integer or bytes. The merge collects one
descriptor per referenced worker-local value in task, candidate and column
order, preserving semantic first occurrence. It *measures* that compact
sequence through the same value resolver into one map, and the projected
bytes join the relations' growth in the admission check. Candidates retain
their local markers until a single *rewrite* composes the local-to-descriptor
map with the promised identifiers. The tables remain untouched throughout.
A round that does not fit is declined and run serially. Reservation occurs
beside the relations' reservations; *append* publishes values in the phase
that cannot fail. The descriptor bound, mapping and byte lifetimes are in
[pending value compaction](design-pending-values.md), and admission is in
[parallel evaluation](design-parallel.md).

**The identifiers are the same at every thread count.** The serial evaluator
interns a value at the commit of the first batch holding it; the merge
interns it under its rank in first-occurrence order among the candidates in
task order. Candidates in task order are the serial derivation order with
duplicates removed -- duplicates of stored tuples, whose values were interned
already, or of earlier candidates of the same worker, which come first -- so
the first occurrence is the same tuple in both, each table gains the same
values in the same order, and, both reserving the stepwise capacity, holds the
same bytes. `relic_stats()` reports the same counts and storage, and every
subsequent memory-limit decision agrees.

**A streamed query never holds a pending value**: a plan that computes can
fail on any binding, so its query is gathered rather than streamed
([evaluation](design-eval.md)), and "every batch is resolved" needs no
exception.

**One table per kind, charged whole.** A second table for computed values
would give `5` computed by a rule and `5` written in a subsequent load two
identifiers, unless every interning consulted both in a fixed order. One
table, one identifier per value, by construction; a computed string spelled
like a symbol shares its pool entry, so `str(foo)` and `"foo"` are one value.

## The hash function and the seed

**The attack.** Tables probe linearly from the slot a key's low hash bits
select, and are fast because clusters are short. An author who can compute
the hash can choose keys whose hashes share low bits, each lengthening the
one cluster every subsequent probe walks. Identifiers follow first
appearance, so a fact file controls every tuple hash; byte strings that
collide under a known function are the textbook flood of the pool. The cost
is quadratic: *M* tuples in one cluster cost *M* probes per membership test
and *M*^2 to load, and *M* values offer enough candidate tuples to craft *M*
colliding ones. No limit stops it: capacity depends only on the count, so the
crafted relation is within every space limit while it runs for hours.

**Determinism does not rest on the seed.** Identifiers are array lengths, not
slots; iteration is by identifier; capacity, and so the memory account and
where `-l memory` refuses, depends on counts; counters count tuples, not
probes; plans and explain output come from counts. So the seed decides where
a key sits and how many probes find it, and nothing an interface reports.

**A secret seed per context.** A context draws a 64-bit seed from
`getentropy(2)` at creation; every table owner copies it and passes it to
every hash. A relation's copy keys its canonical index, its secondary indexes
and its distinct counts; a load's staging hashes a fact as its base store
would, so the commit reuses the hash. The seed is an explicit argument of
every hash function rather than reached through the context, for three
reasons: the table and the hashes stay free of the context and are unit
tested alone; a call site that forgets it fails to compile; and the
alternative, a process-wide seed, is a mutable global, which the library
forbids, and would let input crafted against one context flood another.

**The fold** is multiplicative over a 64-bit state, followed by an
avalanche finalizer that spreads every input bit into the low bits a slot is
chosen by:

```text
state = seed
for each unit u of the key:   state = (state ^ u) * M
hash  = finalize(state)
```

A unit is a byte of a byte string or a 32-bit word of a word sequence; an
integer is its high and low halves, split by shifting. Words are folded as
values, not bytes, so for one seed a table probes in the same order on big-
and little-endian machines. An empty sequence hashes the seeded state, an
ordinary key, which lets an arity-zero relation take the general path.

**The state is twice as wide as a unit so that no difference between two keys
passes the seed untouched.** Multiplying by an odd number carries a
difference in the state's top bit through intact, whatever the other bits
hold, since flipping that bit is the same as adding it, and alters every other
difference by carries that depend on those bits, and so on the seed. A unit
reaches only the lower half, so every difference it makes is altered in a way
only the seed decides before the next unit meets it, and no subsequent unit
can be chosen to cancel it under every seed. A state as wide as a word would
give a word's top bit exactly the seed-independent path: two keys whose words
differ in bit 31 in pairs would collide under every seed. And domain
identifiers carry their type in their top two bits, while a multiply spreads
a difference only upward, so keys differing only in their columns' types
would keep their differences in the top two bits of the state: the 3^n ways
of retyping an n-column key would share at most four hashes whatever the
seed, a flood built from nothing but the choice of which columns hold
integers.
Guarding each place a top bit can be reached would be a list to keep
complete; the wider state has no such place.

**The defence is the secret, not the mixer.** relic runs offline and offers
no oracle: no interface reports a probe count, a slot or a hash, and timing
does not return to whoever wrote the input. Without the seed an author cannot
compute hashes, and no pair of keys collides under every seed. The multiplier
decides how rare the pairs are that collide under some share of seeds. One
with few bits set has short relations modulo 2^64 that let subsequent units
cancel a difference under a fixed share of seeds -- FNV's 2^40 + 435 does --
so `M` (SplitMix64's first multiplier) has its bits spread across the word,
and the rare pairs that remain each spend columns, of which a tuple has at
most 32, and do not compose into a cluster of thousands. A cryptographic keyed
hash would remove them, buy nothing more, and cost every tuple hash on the hot
path.

**Zero is never a seed.** A zero draw is drawn again and a second zero fails
creation, since such a source hands out nothing else. Zero then marks a table
owner never given its context's seed, and every hash asserts it is not given
zero, which checks every owner wherever it hashes.

**Without entropy, creation fails.** Where `getentropy(2)` is missing,
`configure` selects a `compat/` replacement reading `/dev/urandom`. It fails
only when the process has no random source -- a sandbox without a random
device, a filter denying `getrandom(2)` -- and then `relic_create()` returns
`NULL`, as for exhaustion. A context falling back to a fixed seed would be the
vulnerability with a false assurance attached, and the library never aborts.
A caller cannot set the seed: results do not depend on it, a probe test calls
the hash functions with a seed of its choosing, and a configuration field
would cost the binary interface a knob with no use.

## Growth for the life of the context

The symbol pool, the integer table, the use bits, the predicate table and the
name store release nothing. Every distinct value a load, a fact, a query's
constants or an evaluation interns, and every predicate a program, a query or
a rewrite names, stays until the context is destroyed; a discard frees derived
relations and leaves the tables. For the program that is right. For runs it is
a cost: an embedder preparing a query per request over distinct keys grows
the pool by an entry per key, and `s(Y) :- s(X), Y = cat(X, "a")` interns a
string per derivation.

**Run entries are not reclaimed**, because reclaiming them rests on one
invariant -- no identifier a run assigned is reachable once the run ends --
and holding it takes mechanism in every subsystem: a boundary in every table
between the program's entries and the run's, an interning function that
discards the run's before appending, a prologue and epilogue on every run, a
query compiled as a template and instantiated per run (reaching the compiler,
the rewrite and the planner), the rewrite's predicates released with it, and
the interface refusing a run's identifier outside an operation. The growth is
already counted: `max_memory` charges every one of these tables, so a context
that outgrows the limit is refused at the interning that would pass it, with
the limit named. A long-lived owner bounds the growth by loading the program
into a new context and destroying the old; the interface documentation states
this as a property of the context. A workload that needs more wants an
explicit compaction the caller invokes between operations, not reclamation
woven through every run.

**A query changes neither the program nor what a later query answers**, but
the values, predicates and counts it interns persist for the life of the
context. So the memory headroom left under `max_memory`, and the counters
`relic_stats()` reports, may depend on earlier runs.

What leaves the context is bounded by contract instead: a row callback's
values and the bytes `relic_string()` reads for them are borrowed for the
call, and a collected result copies the bytes it holds.

## Failure paths and limits

* **Identifier space.** An index is thirty bits and zero is reserved, so a
  context holds at most 2^30 - 1 distinct byte strings (symbols and strings
  together) and 2^30 - 1 distinct integers. Each table checks its count
  before it stores and refuses with a diagnostic rather than let an index
  run into the type bits and turn one value into another.
* **The memory limit refuses at interning.** The symbol pool, the integer
  table, the use bits and the predicate table are charged to `max_memory`
  whole, and every interning asks the limit before it grows a table. A
  load, an import, a fact or a query whose values or names would pass the
  limit is refused as it interns them, naming the operation, as in
  `relic: error: limit exceeded: memory=N while loading graph.dl`; a value
  evaluation computes is refused at the batch that holds it, naming the
  predicate being derived into.
* **A refusal leaves no partial entry.** A table reserves before it stores,
  and the pool rewinds a refused copy, so a refused interning leaves every
  table holding what it held. What earlier internings of the same operation
  added stays ([growth for life](#growth-for-the-life-of-the-context)).
* **A string too long for the length field** is refused before a byte is
  read.
* **Without entropy no context is created**
  ([the seed](#the-hash-function-and-the-seed)).

## Determinism

* Identifiers follow first appearance within each type, so the same input
  yields the same identifiers; computed values take the identifiers of serial
  first-occurrence order at every thread count.
* Integers and words are hashed as values, so probe order for one seed is the
  same on every platform; the seed decides nothing reported.
* Stripes are chosen by an unkeyed hash and their count is constant, so
  capacities, the memory account and limit decisions are the same in every run
  and at every thread count.
* No identifier or output derives from a pointer value.

## Alternatives not taken

* **A tagged 64-bit cell or a plain 16-byte value** as the column; see
  [domain identifiers](#domain-identifiers).
* **A hash stored beside every generic identifier.** It doubles slot
  storage for all owners. The tag offers mismatch filtering at a quarter
  of that additional cost. A canonical-specific cached-hash candidate
  also targets growth and is evaluated under
  [membership locality](design-membership-locality.md#cached-hashes);
  successful-lookup behavior alone cannot reject or justify it.
* **A generic container** parameterized over keys and values: larger, slower
  and harder to read than a table of inline 32-bit identifiers.
* **A second table for computed values.** It gives one value two identifiers.
* **A step limit or a probe-cluster ceiling** against flooding. A step limit
  caps the quadratic work instead of removing it and needs a new
  configuration field. Clusters under linear probing at three-quarters load
  have a tail long enough that a ceiling no legitimate large table reaches
  must be over a thousand, so crafted input under it slows every lookup a
  thousandfold all the same while legitimate input gains a failure mode.
  The seed removes the attack.
* **A cryptographic keyed hash.** With the seed secret and no oracle, the
  mixer's strength is not what the attack turns on.
* **Reclaiming a run's entries.** See
  [growth for the life of the context](#growth-for-the-life-of-the-context).
