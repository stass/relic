# relic hash seed design

**Status.** Design document for the seed every hash table in the engine
hashes with: why it is drawn at random once per context rather than fixed at
compile time, what that costs determinism (nothing observable), and how the
seed reaches the tables. Forward-looking, like every document under
`specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/design-memory.md` sections 6, 9 and 10 for the
identifier table, the determinism it contributes to and the absence of global
state; `specs/design-relations.md` section 10 for what the relation store
contributes; `specs/architecture.md` sections 45, 46, 61 and 63 for the BSD
interfaces, the portability layer, determinism and resource limits;
`specs/decisions.md` D-4 and D-19.

---

## 1. What this document settles

* the attack: a program author who knows the hash function can make every
  membership test in a relation, an index or the symbol table linear, and
  so make a load quadratic, with no limit to stop it;
* why the fixed seed is not what determinism rests on, and what is;
* the remedy: a 64-bit seed drawn from the operating system once per
  context, held by the context and copied to every table owner, so that no
  input can be crafted against a value its author cannot know;
* how the seed enters the hash, and what the hash's strength does and does
  not contribute;
* where the entropy comes from and what happens when there is none;
* what stays fixed: identifiers, iteration order, plans, `-e` text, table
  capacities and the memory account;
* the tests that hold the property.

It adds no configuration field, no limit and no command-line option.
`relic_config_t` is unchanged.

---

## 2. The attack

Every table in the engine is the open-addressing identifier table of
`specs/design-memory.md` section 6, probed linearly from the slot the low
bits of a key's hash select. Eight tables build on it: the symbol table over
byte strings, the value table over integers, the predicate table, the
canonical full-tuple index of every relation, the secondary index of every
relation, the scratch table that counts a column's distinct values for the
planner, and the adornment and rule tables of the Magic rewrite. Linear
probing is fast because clusters are short, and clusters are short because
hashes are spread. A hash the author of the input can compute lets the author
choose keys whose hashes share their low bits, and every key so chosen
lengthens the one cluster every later probe walks through.

With a compile-time seed the author can compute every hash exactly. Symbol
identifiers are assigned in first-appearance order, so the author controls
each symbol's identifier and through it each domain identifier; a tuple hash
folds domain identifiers; so a fact file fixes every tuple hash in the run.
The cheapest route is not even the tuple: the symbol table hashes the bytes of
a string with a multiplicative fold, and finding byte strings that collide
under a known seed is the textbook flooding attack.

The cost is quadratic in the crafted input. A relation of *M* tuples in one
cluster costs *M* probes per membership test and *M* squared to load; a
binary relation over *M* values offers *M* squared candidate tuples, of which
about *M* land in any one slot, so *M* values are enough to craft *M*
colliding tuples. A hundred thousand values make ten billion probes.

Nothing bounds this. `-l tuples` and `-l memory` bound space, and the table's
capacity is a function of its count alone, so the crafted relation is within
every limit while it runs for hours. Architecture section 63 defines no step
or time limit, and section 61 forbade the one remedy that removes the
attack rather than capping it.

---

## 3. What determinism rests on

Architecture section 61 promises that two runs over the same input produce
byte-identical `-e` output and byte-identical unsorted results, and lists a
fixed hash seed first among the properties that deliver it. The seed is not
one of them. It is a means that was listed beside the ends.

The properties that deliver the promise are the other five: identifiers are
assigned in first-appearance order, relations iterate in insertion order, the
planner breaks ties by source position, Magic names derive from the predicate
and its adornment, and nothing depends on a pointer. Every table in the
engine answers one question, which identifier if any has this key, and is
never walked in slot order: the identifier table exposes a probe cursor and
nothing that enumerates slots, and no owner reads a slot array. So the seed
decides where a key sits and how many probes find it, and nothing else.

The values a run reports that could conceivably depend on the seed do not:

* **identifiers** are the length of the owner's array at insertion, not the
  slot;
* **iteration** is by identifier range;
* **table capacity**, and with it `relic_idtab_bytes()`, the memory account
  and the point at which `-l memory` refuses, is a function of the count
  alone, because the table grows at three quarters full regardless of where
  its entries sit;
* **counters** count tuples inserted, scanned and looked up, not probes;
* **plans and `-e` text** are computed from statistics that are counts.

The golden suite, the differential suite and `-e` therefore see no
difference between a fixed seed and a random one. What a random seed changes
is the number of probes a lookup makes, which no interface reports, and the
one thing the attack needs.

---

## 4. The remedy

A context draws a 64-bit seed from the operating system when it is created
and holds it for its lifetime. Every table owner copies the seed at its own
initialization and passes it to every hash it computes. The hash functions
take the seed as their first argument:

```c
uint32_t relic_hash_bytes(uint64_t seed, const void *bytes, size_t length);
uint32_t relic_hash_words(uint64_t seed, const uint32_t *words, size_t count);
uint32_t relic_hash_word(uint64_t seed, uint32_t word);
uint32_t relic_hash_int64(uint64_t seed, int64_t value);
```

The seed is explicit rather than reached through a context pointer for
three reasons. The identifier table and the hash functions stay free of the
context, so the unit tests exercise them alone.
Every call site that forgets the seed fails to compile, which is the wide
and shallow kind of change the working agreement prefers over a narrow and
clever one. And `specs/design-memory.md` section 10 forbids the alternative
of a process-wide seed outright: a file-scope variable, even one written
once, fails `tests/globals.sh`, and two contexts sharing a seed would let an
input crafted against one flood the other.

The owners are the symbol table, the value table, the predicate table, each
relation, each staging set of a load, and the Magic rewrite. Each already
reaches its keys through an owner pointer in its rehash callback, and the
seed sits in the same structure. A relation's seed keys every table over its
tuples: its canonical index, each of its secondary indexes, and the scratch
table that counts a column's distinct values. Each operation on those
already takes the relation, so none of them holds a copy of its own. A
staging set hashes a fact as the base store it commits to would, and the
commit places the fact by that hash without computing it again
(`specs/design-facts.md` section 4.3), so the two must hold the same seed,
which both copy from the context.

A caller cannot set the seed. There is nothing a caller could want from a
chosen seed that a random one does not give: results do not depend on it,
and a test of probe behaviour calls the hash functions with a seed of its
choosing directly. Adding a field to `relic_config_t` would break the
library's binary interface for a knob with no use.

---

## 5. How the seed enters the hash

The hash is a multiplicative fold over words or bytes, then the avalanche
finalizer that spreads every input bit into the low bits a slot is chosen
by. The fold's state is 64 bits wide, twice the widest unit, and it starts
from the seed:

```text
state    = seed
for each unit u:   state = (state ^ u) * M
hash     = low 32 bits of finalize64(state)
```

`M` is an odd 64-bit constant whose bits are spread across the word, the
first multiplier of SplitMix64's mixer.

An empty sequence hashes the seeded state, which is an ordinary key and not
a special case. A word hashes as the sequence of that one word, and an
integer as the sequence of its high and low 32-bit halves, split by shifting
so that a table probes in the same order on a big-endian machine as on a
little-endian one for the same seed.

The state is wider than a unit so that no difference between two keys can
be carried past the seed unchanged. Multiplying by an odd number carries a
difference in the state's top bit through unchanged whatever the other
bits hold, since flipping that bit is the same as adding it, and changes
every other difference by carries that depend on the other bits, and so on
the seed. A unit reaches only the lower half of the state, so every
difference it makes has been changed in a way only the seed decides before
the next unit meets it, and no later unit can be chosen to cancel it under
every seed.

A state as wide as a word would give a word's top bit exactly that path,
and domain identifiers carry their type in their top two bits. Two keys
whose words differ in bit 31 in pairs would collide under every seed, and
since a multiply spreads a difference only upward, keys differing only in
their columns' types would keep their differences in the top two bits of
the state: the 3^n ways of retyping an n-column key would share at most four
hashes whatever the seed, a flood built from nothing but the choice of
which columns hold integers. Guarding each place a top bit can be reached
would be a list to keep complete; the wider state has no such place.

The defence is the secret, not the mixer. relic runs offline over a file
or a caller's buffers and offers no oracle: no interface reports a probe
count, a slot or a hash, and timing is not returned to whoever wrote the
input. An author who cannot learn the seed cannot compute the hashes, and
no pair of keys collides under every seed.

What the multiplier decides is how rare the pairs are that collide under a
share of the seeds. For a later unit to cancel a difference, the multiplies
between must leave it small enough for a unit to reach, and that takes a
relation between the difference and the multiplier modulo 2^64. A multiplier
with few bits set has short ones: FNV's prime, 2^40 + 435, less 435 and
squared is a multiple of 2^64, so three words can be chosen whose
differences offset one another under about one seed in a thousand, and a
search over differences of a few bits each finds hundreds of such channels.
`M` has no relation a search of the same differences finds, and for two
words only 838,328,728, twice it and their negations, among the differences
below 2^32 in magnitude, are multiplied into another that small, each then
cancelled only under the seeds whose bits give it the right carries. Such
pairs do not compose into a flood: each spends columns, a tuple has at most
thirty-two, and the keys that collide with one another under a given seed
stay a handful while a crafted cluster would need thousands. A keyed hash of
cryptographic strength would remove them, buy nothing more, and cost every
tuple hash on the hot path of evaluation, so the fold stays.

No context holds a seed of zero. A draw of zero is drawn again, and a
second zero fails creation, since a source that returns zero twice is
handing out nothing but zeros. That makes
zero the mark of a table owner that was never given its context's seed, and
the hash functions assert that they are not given it, which checks every
owner wherever it hashes.

---

## 6. Where the entropy comes from

The seed is read with `getentropy(2)`, which every platform relic builds on
provides: OpenBSD, FreeBSD, macOS, glibc and musl. It is a BSD interface in
the sense of architecture section 45, it needs no file descriptor and no
state, and it cannot be made to return fewer bytes than asked. Where a
platform lacks it, `configure` selects a replacement from `compat/` that
reads `/dev/urandom`, under the rule of section 46 that no engine source
contains a platform test.

`getentropy(2)` fails only when the system has no random source to offer
the process: a sandbox built without a random device, or on Linux a kernel
older than 3.17 or a seccomp filter that denies `getrandom(2)`, which glibc
and musl implement it with. Then `relic_create()` returns `NULL`, as it
does when the context cannot be allocated. A context that fell
back to a fixed seed would be the vulnerability with a false assurance
attached, and a library never aborts, per D-6; failing creation is the one
remaining honest outcome. The failure is not reported through the diagnostic
callback because there is no context to hold one, which is the same gap
allocation failure has. The contract of `relic_create()` in `<relic.h>`,
which names exhaustion as the only way creation fails, names the missing
random device as the second.

---

## 7. What stays fixed

Nothing an interface reports changes when the seed does.

* `make check` compares output byte for byte and passes unchanged, because
  no golden file records a hash.
* `make check-random` compares the engine against its reference evaluators
  and passes unchanged, because the seed is not among the inputs it varies.
* `-e` output is identical across runs, because plans are computed from
  counts.
* The memory account charges the same bytes at the same points, because
  capacities depend on counts.
* Identifiers, tuple order and diagnostics are identical across runs.

The seed is the one value in a run that is drawn from the environment, and
this document is the record that it is drawn deliberately.

---

## 8. Tests

* **Reproducibility within a seed.** Equal input with equal seed hashes
  equally, for bytes, words and integers; this is the existing determinism
  scenario with the seed made explicit.
* **Sensitivity to the seed.** The same input under two seeds hashes
  differently for at least one of a few keys, so a seed that is silently
  ignored is caught.
* **Endian independence.** A word sequence hashes to the same value on every
  platform for the same seed; the existing scenario, with a chosen seed
  rather than the compile-time basis.
* **Flooding is a property of the seed.** A set of keys crafted to share a
  slot under one seed, inserted into the identifier table with that seed,
  probes through a cluster as long as the set; the same keys inserted under
  another seed probe through short clusters. This is the regression test for
  the attack, at the level where the test controls every hash, and it fails
  against a table hashed with compile-time constants by construction.
* **Two contexts draw two seeds.** Two contexts created in one process hold
  different seeds, which holds with probability one minus 2^-64 and catches
  a seed read once and cached, a seed of zero and a seed read from something
  that is not random.
* **Output is seed-independent.** The golden suite and the differential
  suite pass unchanged; they are the test, and no new one is needed.
* **Entropy failure.** `relic_create()` returns `NULL` when `getentropy(2)`
  fails, exercised the way the exhaustion tests fail allocation: the library
  sources of that test program are compiled with the call renamed, and the
  test supplies a replacement that fails on demand.

---

## 9. What is deliberately absent

* **A step limit.** `-l steps=N` would need a field in `relic_config_t` and
  so a new major version of the shared library, and it caps the quadratic
  work rather than removing it.
* **A probe-cluster ceiling.** Linear probing at three quarters load has a
  cluster-length tail with base near 0.96, so a ceiling that a legitimate
  ten-million-entry table never reaches must be over a thousand. An input
  crafted to stay under it still slows every lookup a thousandfold, and a
  legitimate input gains a failure mode it never had. A ceiling bounds the
  damage; the seed removes it.
* **A stronger hash.** With the seed secret and no oracle, the mixer's
  strength is not what the attack turns on; see section 5.
* **A caller-chosen seed.** See section 4.
