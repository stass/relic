# relic memory design

**Status.** Design document for the storage primitives every other subsystem
sits on: arenas, growable arrays, the open-addressing identifier table, the
symbol pool and the value table. Forward-looking, like every document under
`specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 18, 19, 20, 46, 47 and 63;
`relic-plan.md` decisions D-3, D-4, D-6, D-7 and D-14.

---

## 1. What this document settles

Five questions, once, so that no later subsystem answers them again:

* which allocations are grouped by lifetime and which are individually owned;
* what a caller must do when an allocation fails;
* how a growable array is resized without repeating overflow arithmetic;
* how a byte string or a value becomes a small integer identifier;
* what makes identifier assignment reproducible across runs and platforms.

It does not cover relation and index storage. Those are sized by the data
rather than by the program, they are what a memory limit is enforced against,
and they get their own document with the tuple store. The two arenas that
hold an operation's working state are charged to the same limit, for the
reason `specs/design-planning-cost.md` section 6 gives.

---

## 2. Lifetimes

Allocation in relic falls into two kinds, and the kind is decided by lifetime
rather than by size.

**Grouped by phase.** Symbols, parsed rules, plans and join state are created
in large numbers, are never freed individually, and all die at the same
moment. They are allocated from an arena, which frees them in one operation.
Seven arenas exist, one per lifetime:

| Arena | Holds | Released when |
| --- | --- | --- |
| context | symbol bytes, value payloads, predicate definitions | the context is destroyed |
| program | rules, parsed terms | a new program replaces the loaded one |
| analysis | the dependency graph and its component search | the analysis ends |
| schedule | the evaluation order: components, their members and rules | a new schedule replaces it |
| query | the query's compiled rule | the query is destroyed |
| rewrite | the Magic rewrite of one query's program | the query has run |
| evaluation | plans, cursors and transient join state | the fixed point ends |

The evaluation arena, and the arena of a query's rewrite that
`specs/design-magic.md` section 11 describes, are released when their
operation ends and are charged to `max_memory`; the others hold what the
caller wrote and are not, per `specs/design-planning-cost.md` section 6.

**Individually owned.** Relation tuples and indexes outlive individual
queries, must be able to shrink, and are accounted against a limit. They are
growable arrays (section 5) with explicit failure handling and are never
arena allocated. The base stores that hold a program's facts are relations like
any other, in the account with the relations evaluation derives
(`specs/design-facts.md`); a discard empties the derived relations and drops
the base stores' secondary indexes, and never shrinks a base store's tuples.
The store records whether it holds anything a discard removes, so a discard
with nothing derived to remove returns at once. A query's answer relation is
owned the same way but by the query object rather than the context, and is
released with the query, per `specs/design-query.md`.

**What the account bounds** is what the program's stores hold -- its facts
and what it derives, with their indexes -- and what running the program does,
in the two charged arenas. It does not bound what the caller wrote: the
program's rules and queries, the text a load reads, and the staging a load
holds its facts in until it commits. Staging is individually owned, grows by
doubling, and is released when the load returns, whatever the outcome; it is
proportional to the text being loaded, as the text and the program arena
are.

The growable arrays inside the symbol and value tables are the third case, and
they are individually owned for a mechanical reason rather than a conceptual
one: an arena cannot resize a block, and an array that doubles would otherwise
leave every superseded generation of itself in the arena. Their *contents*
--- the symbol bytes --- do live in the context arena, which is what keeps a
pointer returned by `relic_string()` valid for the life of the context even
though the array indexing it has since moved.

---

## 3. The arena

An arena is a list of chunks. Each chunk is one allocation holding a small
header followed by a payload, and allocation is a bump of the head chunk's
`used` offset. Nothing is freed until the arena is reset, at which point every
chunk is released together.

```c
relic_arena_t arena;

relic_arena_init(&arena);
p = relic_arena_alloc(&arena, n);
...
relic_arena_reset(&arena);
```

`relic_arena_reset()` returns the arena to its just-initialized state and
releases all of its memory, so it serves both as a reset between phases and as
the teardown operation. There is no per-allocation free, and there will not
be one: the point of the arena is that a phase's allocations are forgotten
together.

**Chunk sizing.** The first chunk holds 4 KiB of payload and each subsequent
chunk doubles, to a cap of 256 KiB. A request larger than the chunk size gets
a chunk of exactly its own size.

**Where a new chunk is linked.** A chunk allocated because the request did not
fit is normally pushed at the head, which abandons whatever space remained in
the previous head. For an oversize request that would be the wrong trade: a
program that allocates one large block and then continues allocating small
ones would get a fresh chunk for every small allocation afterwards, because
the oversize chunk at the head is full. An oversize chunk is therefore linked
*after* the head, leaving the partially used chunk in front of it where the
next small allocation will find its remaining space.

**Alignment.** Every allocation is aligned for any object relic creates, by
rounding the requested size up to `_Alignof(max_align_t)` and placing the
payload at the first correctly aligned offset after the chunk header. A
request of zero bytes is rounded up in the same way, so it yields a distinct
non-null pointer and a null return means failure and nothing else.

---

## 4. The sticky failure flag

`relic_arena_alloc()` returns `NULL` when it cannot obtain memory, and also
sets a failure flag on the arena that no further allocation clears. Once the
flag is set the arena refuses every subsequent request without calling the
allocator.

The contract has three parts, and each exists for a reason:

1. **Callers check the returned pointer**, exactly as they would check
   `malloc()`. Nothing about the arena changes ordinary defensive C.

2. **A helper deep in a construction may return early without inventing an
   error path.** If it allocated ten nodes and the third failed, the seventh
   allocation also returns `NULL`, so the helper does not have to thread a
   status through every level to stop the remaining work from running against
   a half-built structure.

3. **Each compilation phase confirms the flag at its boundary.** This is what
   makes the first two safe. A missed pointer check cannot turn into a
   silently truncated program, because the phase that owns the arena asks
   `relic_arena_failed()` before it hands its output to the next phase and
   fails the whole operation if the answer is yes.

Refusing allocations after a failure is deliberate rather than incidental. It
keeps the flag from being contradicted by a later small allocation that
happens to succeed, and it stops an unwinding path from issuing a storm of
requests to an allocator that has already said no.

**The flag belongs to a phase, not to a lifetime.** The three parts above
describe an arena that is reset when its phase ends, and the reset is what
clears the flag. The context arena is the exception: it lives as long as the
context and is never reset, so a flag left standing on it would refuse every
later request and turn one transient exhaustion into a context that can never
intern anything again --- a program that reported the failure and carried on,
which is exactly what a REPL or a long-running embedder does, would find every
subsequent operation failing for a reason that no longer applies.

Context-lifetime allocation therefore goes through one function,
`relic_context_alloc()`, which reports the exhaustion and clears the flag,
because by then the failure has been observed and acted on. It is a funnel
rather than a rule to remember: there is no way to allocate from the context
arena that skips it, so no future caller has to know any of this. What that
funnel gives up is the second part of the contract above --- a half-finished
construction no longer stops by itself --- and it gives it up safely, because
context-lifetime allocations are individual objects whose callers check the
pointer they get. The deep construction that wants the early return is the
parsed program, and it is built in the program arena, which is reset.

Exhaustion is reported as an ordinary diagnostic and a `-1` return. The
library never aborts: it must not terminate its host.

---

## 5. Growable arrays

Every growable array in the tree resizes through one function:

```c
items = relic_grow_array(items, &capacity, count + 1, sizeof(*items));
if (items == NULL)
	return (-1);
```

It returns the array unchanged when the existing capacity already suffices,
and otherwise doubles it --- starting at eight items, and never below the
requested count --- rejecting a product that would overflow. On failure it
returns `NULL` and leaves the caller's array and capacity untouched, so a
failed growth loses nothing that was already there.

The point is not that resizing is hard. It is that the doubling arithmetic,
the overflow check and the failure discipline are written once rather than at
every array in the engine, while the array itself stays a plain typed pointer
that the surrounding code indexes directly.

**Where an array lives.** An array whose bytes reach `RELIC_ARRAY_MAP_BYTES`,
one mebibyte, is given an anonymous private mapping of its own; a smaller one
is a heap block. A small block grows by `realloc()`. Every other growth maps
the new size, copies and unmaps the old, so the pages of an outgrown
generation go back to the system at once instead of staying resident as a
freed heap block of that size may. The identifier table of section 6
allocates its slot and tag arrays by the same rule.

A header in front of the first item records which storage an array got: the
length of its mapping, or zero for a heap block, sealed against the record's
own address. An array is released by

```c
relic_free_array(items);
```

which checks the seal and then unmaps or frees. The header is what makes the
rule hold without any caller's help. No owner tracks which of its arrays are
mapped, and none can release one the wrong way unnoticed: `free()` on an
array of any size is handed a pointer the allocator never returned, which
AddressSanitizer rejects the first time it runs, not only once some input
makes the array large. A header damaged by a stray write fails its seal and
stops the process, rather than unmap whatever length it claims; under
AddressSanitizer the header is unaddressable, as a heap block's redzone is,
so the stray write itself is reported.

---

## 6. The identifier table

Interning, duplicate elimination and every index in the engine reduce to the
same question: given a key, which 32-bit identifier, if any, has already been
assigned to it? One open-addressing table answers it for all of them.

```c
struct relic_idtab {
	uint32_t	*slots;		/* 0 marks an empty slot */
	uint8_t		*tags;		/* per slot when tagged, else NULL */
	size_t		 capacity;	/* a power of two, or zero while empty */
	size_t		 count;
	bool		 tagged;
};

void	relic_idtab_init(relic_idtab_t *table, bool tagged);
```

A slot holds an identifier and nothing else --- no key, no pointer, no hash.
That is the entire reason for a purpose-built table rather than a generic
container: the key lives wherever its owner already keeps it, so the table
costs four bytes per entry and the owner pays nothing twice.

Storing the hash beside the identifier would let a probe pass a mismatch
without asking the owner, and let the table grow without calling back into
it. `specs/design-evaluation-cost.md` section 5.2 measured that slot on the
closure workloads: their duplicate checks are almost all successful
searches, which read the matching tuple either way and pass few
mismatches, and the doubled slot array raised the peak for a mixed result
in time. The slot stays an identifier.

**A tag is the cheaper form of the same thing.** A table created tagged
keeps one byte per slot in `tags`, beside the slot array: the top eight bits
of the hash its identifier was placed under. The bottom bits of a hash choose
its first slot, so a tag tells apart keys that share a slot's
neighbourhood, and `relic_idprobe_next()` passes an occupied slot whose tag
differs from the probe's without returning it. A search for an absent key
then asks its owner about one mismatching slot in 256 instead of each one it
visits. The tag array is a quarter of the slot array rather than a doubling,
and it is read only where the slot array is, so a successful search pays one
byte more per visited slot and nothing else. Growth still asks the owner for
each identifier's hash; a tag is not a hash.

Whether a table is tagged is decided by its owner when it is created, and
kept for the table's life:

| Table | Its searches | Tagged |
| --- | --- | --- |
| symbol pool | unsuccessful for every new symbol; the bytes far away | yes |
| value table | as the symbol pool | yes |
| index keys | a probe compares stored key values | yes |
| a load's staging sets | unsuccessful for every distinct fact | yes |
| relation (the canonical index) | successful, per the measurement above | no |
| a distinct count | unsuccessful for every distinct value; each candidate a tuple | yes |
| predicate table, rewrite tables | small | no |

**Identifier zero is never assigned.** Symbol identifiers and domain
identifiers both start at one, which is what lets a zeroed slot mean "empty"
with no separate occupancy bitmap and no sentinel that a caller could collide
with.

**Probing.** Slots are probed linearly from `hash & (capacity - 1)`,
wrapping, and in a tagged table a slot whose tag is not the hash's is passed
over. The sequence exists in exactly one place, a cursor, and both lookup
and insertion walk it:

```c
relic_idprobe_open(&probe, &table->index, hash);
while (relic_idprobe_next(&probe, &id)) {
	if (symbol_has_bytes(ctx, id, bytes, length))
		return (id);		/* already interned */
}
/* The probe stopped on an empty slot: the key is not in the table. */
```

The comparison stays at the call site, where the reader can see what makes two
keys equal, while the arithmetic that a second hand-written probe loop would
get subtly wrong stays in the table. Insertion re-probes from the hash rather
than reusing the cursor, so no caller has to know that growth would have
invalidated it.

**Growth.** The table doubles when it would exceed three quarters full, which
keeps a probe short and, more importantly, guarantees an empty slot exists so
that a probe always terminates. Rehashing needs each stored identifier's hash,
which only the owner can compute, so the owner supplies a function that maps
an identifier back to its hash. That indirect call happens once per entry per
doubling and never on a lookup.

**Reserving and placing are separate operations, and the order matters more
than it looks.** `relic_idtab_reserve()` makes room for the entries about to be
placed and is the only half that can fail; `relic_idtab_place()` then stores the identifier
and cannot. An owner reserves *before* it creates the thing being indexed, so
that the step which can fail runs while there is still nothing to leave
behind.

A reservation takes the number of entries to make room for, and at most that
many places may follow it. The table grows at most once, when the entries
would take it past three quarters full, straight to the smallest power of two,
at least twice its size, that holds them, rehashing each entry once; a load's
commit reserves for all of a base store's new facts this way before it places
any. A pure function gives the bytes the slots would occupy after such a
reservation, which is what lets a caller bound by the memory limit project a
growth before it makes it.

The other order is the one to understand, because it is the one that looks
harmless. Storing first and indexing second means a failed growth leaves an
entry in the owner's array that no lookup can reach. For an interning table
that is not a lost entry, it is a broken invariant: the next intern of the
same key does not find it, so it stores a *second* entry with a *second*
identifier, and from then on two occurrences of one symbol compare unequal.
Nothing reports this. A query simply returns the wrong tuples, for the rest of
the context's life, because of one transient allocation failure that was
correctly reported at the time.

Splitting the operation makes that state unrepresentable rather than merely
unlikely, which is why it is worth two functions instead of one.

**No deletion.** Nothing in v1 removes an interned symbol or value, so the
table has no tombstones. This is worth stating rather than leaving implicit:
tombstones are what makes linear probing delicate, and their absence is a
property of the workload, not an oversight. A future table that must delete
should be a different type rather than a flag on this one.

---

## 7. The symbol pool

`lib/symbol.c` maps a byte string to a `uint32_t` symbol identifier.

```c
struct relic_symbol {
	const char	*bytes;		/* NUL-terminated, in the context arena */
	size_t		 length;	/* bytes before the terminator */
};
```

Symbols are stored in a growable array indexed by `id - 1` and found through
an identifier table. The bytes themselves are copied into the context arena,
where they never move, so the pointer `relic_string()` returns stays valid
until the context is destroyed even after the array has been reallocated
several times.

Length is explicit because a string literal may contain NUL bytes: the lexer
is byte-oriented and a string holds arbitrary bytes. A terminator is stored
anyway, after the recorded length, so that a caller that has no use for the
length can treat the result as a C string.

Symbols and string literals share one pool. They are distinguished by the
*value* that refers to them, not by the pool, so the symbol `foo` and the
string `"foo"` intern to one symbol identifier and to two different domain
identifiers. Storing one copy of the bytes is the point; keeping the types
distinct is the value table's job.

---

## 8. The value table

`lib/value.c` maps a value to a `relic_dom_t`, the opaque 32-bit domain
identifier that a tuple column holds.

```c
struct relic_value {
	enum relic_type	type;		/* symbol, string or integer */
	union {
		uint32_t	id;	/* a symbol identifier */
		int64_t		integer;
	} v;
};
```

Values are stored in a growable array indexed by `dom - 1` and found through a
second identifier table. Interning happens here for every type, integers
included, so that the parser and `relic_fact_add()` cannot disagree about what
the canonical form of a value is: there is only one path to a domain
identifier and it is this one.

**Hashing and comparison read fields, never bytes.** `relic_value_t` contains
a union and padding, so hashing its representation would depend on bytes no
assignment defines. The hash combines the type and the live union member; the
equality test compares the type and then that member. This is the same hazard
that decision D-3 rejects a plain 16-byte value cell for, and the reason it
does not reach the tuple store is that a tuple stores domain identifiers
rather than values.

**A value is normalized as it enters the table.** The padding and the unused
half of the union are zeroed and only the member the type selects is copied,
so a stored value has one definite representation. This is not tidiness: a
caller fills in the fields the interface documents and leaves the rest holding
whatever its stack held, and `relic_value_of()` hands back what was stored.
Without normalization the library would retain and return bytes of its host's
memory that were never given to it, and the guarantee would hold only for the
callers that remembered to zero their locals first.

**Ordering.** Two orders exist over domain identifiers, and they are not the
same order:

* the **language order**, which the comparison operators use. It is defined
  only between values of one type --- numeric for integers, bytewise for
  symbols and for strings --- and reports *unordered* for a mixed-type pair,
  which the evaluator turns into a filtered-out binding rather than an error,
  because a column may legitimately hold more than one type.

* the **total order**, which `-s` uses to sort output. It ranks by type first,
  integer before symbol before string, and falls back to the language order
  within a type. It exists to stabilize output and is not visible to the
  language.

Bytewise rather than locale collation, so that a result never depends on the
environment.

**Nothing may order values by domain identifier.** Identifiers are assigned in
order of first appearance, so their numeric order records the order the input
mentioned things in and nothing else. Both order functions go through the
value table, and that is the only correct way to compare two values.

---

## 9. Determinism

Identifier assignment must be reproducible, because the golden test suite
compares output byte for byte across runs, machines and object formats.

* **The hash seed decides nothing that is reported.** A table is never
  walked in slot order, so the seed decides where a key sits and how many
  probes find it and nothing else; it is drawn at random once per context,
  per `specs/design-hash-seed.md`, and no identifier, capacity or output
  depends on it.
* **Integers are hashed arithmetically, not by reinterpreting their bytes**,
  so a table on a big-endian machine probes in the same order as one on a
  little-endian machine.
* **Identifiers are assigned in order of first appearance.** The counter is
  the array length, so the same input yields the same identifiers.
* **No identifier or output is derived from a pointer value.**

---

## 10. No mutable global state

A `relic_t` owns everything it allocates and two contexts share nothing, which
is what lets an embedding program hold several without a lock. The library
therefore contains no mutable global or file-scope variable at all --- not a
cache, not a counter, not a lazily initialized table.

This is checked rather than promised. The test suite inspects the built
library's symbol table and fails if any symbol lives in a writable section. A
constant table is fine and lands in read-only data; a `static` counter added
for debugging fails the build, and so does a `_Thread_local` one, which two
contexts on one thread would share just as surely.

---

## 11. What is deliberately absent

* **A free list, and per-allocation free in the arena.** An arena whose
  allocations can be individually released is a general allocator with extra
  steps.
* **Chunk retention across a reset.** `relic_arena_reset()` releases
  everything, so an arena reset in a hot loop re-enters the allocator. No such
  loop exists yet; retaining the head chunk is a measured optimization if one
  appears, not a guess now.
* **A generic container.** The identifier table stores 32-bit identifiers
  inline and knows nothing about keys. A container parameterized over key and
  value types would be larger, slower and harder to read than the few hundred
  lines it replaced.
* **Reference counting, and any form of shared ownership.** Every allocation
  in the engine has exactly one owner and one lifetime.
