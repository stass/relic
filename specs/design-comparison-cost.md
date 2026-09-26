# relic comparison cost design

**Status.** Design document for the six changes that close the gaps the
engine comparison of `specs/design-bench.md` measured against the Souffle
interpreter, DuckDB and SQLite: how a row reaches the output, when an answer
is streamed rather than stored, which adornments a free query keeps, what a
secondary index stores for a key, how a symbol and a value are represented,
and how a large array grows. Forward-looking, like every document under
`specs/`; `STATUS.md` records what is implemented.

**Companions.** `specs/architecture.md` sections 18, 19, 20, 22, 33, 39, 43,
47, 52 and 62; `specs/design-cli.md` sections 4 and 7 for the formats the
writer keeps; `specs/design-query.md` sections 2 and 3 for the answer
relation this design bypasses; `specs/design-magic.md` sections 3, 4, 5 and
9.3 for the rewrite this design narrows; `specs/design-index.md` section 3
for the index this design extends; `specs/design-memory.md` sections 6, 7
and 8 for the tables this design changes; `specs/design-relations.md`
section 3 and `specs/design-facts.md` section 4.3 for the growth and the
commit; `specs/design-evaluation-cost.md` sections 2 and 5.2 for the earlier
measurement this one continues; `relic-plan.md` decisions D-3, D-4, D-17 and
D-22 to D-24.

---

## 1. What this document settles

* where the time goes on the four workloads where relic trails or ties
  another engine, measured by phase and by profile;
* that a row is formatted into a buffer the tool owns and written in
  blocks, and what that keeps of the write-failure contract;
* that a query whose head keeps every variable of its body is answered
  from its rule's derivations directly, with no answer relation in
  between, and why that answer is the same set in the same order;
* that a predicate reached with every column free subsumes every other
  adornment of it, and that a rewrite left with no bound column is no
  rewrite: the query reads the program's own relations;
* that a secondary index stores each key's values beside its chain, and
  that an identifier table can carry a tag byte per slot that lets a
  probe pass a mismatch without asking the owner;
* that a domain identifier carries its type, so a symbol value is its
  symbol identifier and the value table holds integers only, and how a
  symbol's bytes are packed;
* that arrays past a page threshold are mapped rather than malloced, so a
  growth leaves nothing resident behind it;
* what each change does to the accounting, the counters, explain and the
  determinism promise, and in what order the changes land.

It does not revisit the evaluator's join loop, the planner's order or the
canonical index's four-byte slot, which `specs/design-evaluation-cost.md`
settled and which the measurements below do not implicate.

---

## 2. The gap, measured

`bench/compare/run.sh` at `BENCH_SCALE=1` on an Intel Xeon W-3245, macOS
26, Apple clang 21, relic configured with `--disable-shared` at `-g -O2`,
one run of each cell on a machine busy with other work. The counters are
exact; the seconds are indicative and were read for their ratios. relic
was timed three ways: as the harness runs it, with `-n` so that it wrote
no rows, and with `-c` so that it imported and evaluated nothing.

| Workload | relic | relic `-n` | relic `-c` | Nearest rival |
| --- | --- | --- | --- | --- |
| `chain-closure` | 1.56 s | 0.71 s | 0.00 s | Souffle interpreter 1.35 s |
| `tc-sparse-cyclic` | 0.82 s | 0.48 s | 0.00 s | Souffle interpreter 0.83 s, DuckDB 0.78 s |
| `andersen` | 0.67 s | 0.60 s, 0.26 s with `-a` | 0.03 s | Souffle interpreter 0.41 s |
| `facts-join` | 0.30 s | 0.28 s | 0.21 s | DuckDB 0.30 s, SQLite 0.41 s |

| Workload | relic peak | relic live heap | Nearest rival's peak |
| --- | --- | --- | --- |
| `chain-closure` | 100 to 134 MB | 67 MB | Souffle interpreter 46 MB |
| `facts-join` | 47 MB | | SQLite 9.7 MB, DuckDB 38 MB |

Three causes account for the time and two for the memory. None of them is
in the join loop.

**Cause 1: writing the rows costs more than deriving them.** The tool's
`write_row()` writes every value and every separator with one stdio call,
`fputs()`, `fwrite()` or `fputc()`, and formats an integer with
`snprintf()`. On macOS each stdio call takes and releases the stream's
lock, and a sampled profile of `chain-closure` puts 58 per cent of its
samples under `receive_row()`, almost all of them in the lock and unlock
slow paths under `flockfile()` and `funlockfile()`; `tc-sparse-cyclic`
puts 40 per cent there, with `vfprintf()` and `ultoa()` beside the locks.
Souffle writes its answer to a file in the same harness and pays for it
too, so the comparison is fair and relic's writer is simply slow: without
output, relic evaluates both closures faster than the interpreter.

**Cause 2: a free query is rewritten into three copies of its relation.**
The query `pointsTo(X, Y)?` adorns `pointsTo` free, `ff`, and the rewrite
copies the four rules for `pointsTo$ff`. Two of those rules call
`pointsTo` twice, and the planner places `load` and `store` before the
first call, so that call is adorned `bf` and the second `fb`; the rewrite
then generates `pointsTo$bf` and `pointsTo$fb` with their magic and
supplementary predicates, nine relations in one recursive component in
place of one:

| | Rewritten | Whole program, `-a` |
| --- | --- | --- |
| Iterations | 47 | 32 |
| Derivations | 1,741,012 | 606,055 |
| Tuples inserted | 472,445 | 181,162 |
| Tuples held | 429,560 | 138,277 |
| Time without output | 0.60 s | 0.26 s |

Everything `pointsTo$bf` and `pointsTo$fb` derive, `pointsTo$ff` derives
too, since it derives all of `pointsTo`. The bound copies exist because
the rewrite treats every call of an adorned rule alike, and the design's
reason for rewriting the rules of a free adornment at all, that a bound
call inside them propagates demand, is exactly what costs here: the demand
propagated is for a relation that is being computed in full beside it.

**Cause 3: the import is the whole of `facts-join`.** The query joins
8,786 rows and derives 17,572; the remaining 0.21 s of 0.30 read 300,000
rows and interned 301,000 symbols. At scale 10 the import alone takes
3.6 s, 1.2 us per row, and 351 MB, 117 bytes per row. A profile of that
import is two thirds interning: `relic_idprobe_next()` and `memcmp()` are
the top two symbols, then `relic_value_intern()`, `relic_symbol_intern()`,
`relic_hash_bytes()`, and the two rehash callbacks. A slot holds an
identifier and nothing else, so every occupied slot a probe visits costs a
read of the entry and a `memcmp()` of its bytes, both in memory a
300,000-symbol table no longer keeps in cache, and an unsuccessful search,
which every new symbol makes, visits every slot of its run. A symbol is
then interned twice, once into the pool and once into the value table, and
each row is hashed four times on its way in: by the staging set, by the
commit's membership check, and twice by the base append.

**The import's memory is the symbol machinery.** The heap of the scale-10
import before teardown holds 302 MB: two arrays of 64 MB, the symbol
entries and the value entries at 16 bytes each and a capacity of four
million; 35 MB of columns; three 16 MB slot arrays, the relation's
canonical index and the two interning tables; and 52 MB of arena blocks
holding 27 MB of symbol bytes, each string aligned to sixteen. The tuple
store is 17 bytes per row; the symbol costs 70. D-3 measured the
symbol-heavy case at about 30 bytes per distinct symbol, and the
difference is the second table, the alignment and the doubling.

**The closures' memory is the answer copy and the tables' old
generations.** `chain-closure` derives 2,001,000 paths and inserts
4,002,000 tuples: the query rule `$q0(X, Y) :- path$ff(X, Y)` scans the
relation and inserts every tuple into the answer relation, with its own
canonical index, so the live heap at the end is two relations of 33.6 MB,
and `relation_insert_hashed()` and `relic_hash_words()` lead the
evaluation's own profile. The reported peak, `ru_maxrss` as hyperfine
reads it, is higher than the live heap because an identifier table grows
by allocating its new slots, rehashing and freeing the old, and a freed
block of that size stays resident on macOS until the allocator reuses it:
a test that grows a 64 MB array by `realloc()` reports a peak of 64 MB,
and the same growth by `malloc()`, copy and `free()` reports 128 MB. The
column arrays grow by `realloc()` and cost nothing beyond their slack; the
slot arrays leave every generation behind.

**What is not in the gap.** The delta placement, the frontier, batched
insertion and the four-byte slot of the canonical index behave as
`specs/design-evaluation-cost.md` measured: on the closures, relic without
output is at or ahead of the interpreter, and the duplicate check is where
that design left it. The planner's order is not implicated either; cause 2
is a property of the rewrite, not of the order it was handed.

---

## 3. The row writer

The tool formats every row into a buffer it owns and writes the buffer in
blocks. The formats of `specs/design-cli.md` section 4 are unchanged to the
byte; what changes is how many calls produce them.

```c
struct output {
	FILE		*stream;
	char		*buffer;	/* OUTPUT_BUFFER_SIZE bytes */
	size_t		 filled;
	bool		 failed;	/* a write failed; reported once */
	...
};
```

**One buffer, one call per block.** `OUTPUT_BUFFER_SIZE` is 64 KiB. A
value is appended to the buffer by a formatter for its type: a symbol's
bytes are copied; a string literal is escaped byte by byte into the buffer
as section 4.1 spells it; an integer is rendered by a decimal routine of
the tool's own, at most twenty digits and a sign, into the buffer
directly. A separator and a row's newline are single bytes appended the
same way. Before appending anything the writer checks that it fits, and
when it does not it flushes: one `fwrite()` of the filled prefix, checked.
A value longer than the buffer, which only a string can be, is escaped in
pieces that each fit, so no value is ever split by anything but a flush
boundary and the output is the same bytes whatever the boundaries fall on.

**stdio stays the transport.** The stream is standard output or the file
`-o` opened, exactly as now; the buffer sits in front of it, so `-o`, the
truncation rules and the close of `specs/design-cli.md` section 6 do not
change. A flush is one `fwrite()`, which takes the stream's lock once per
64 KiB rather than once per byte. The stream's own buffer is set to none
with `setvbuf()`, so the block goes to `write(2)` whole and is not copied
a second time.

**Write failures keep their contract.** Section 7 promises that a failed
write is reported once and ends the run with status 1. Each flush checks
`fwrite()`'s return; the first short write sets `failed`, and every later
append and flush returns at once without touching the stream, so a closed
pipe costs one failed call and no further formatting. The final flush of a
query, its `fflush()` and the close at the end of the run are checked as
they are now. A row is delivered to the writer by the callback of section
4, which returns nonzero once `failed` is set, so the query stops
streaming as it stops now when a write fails.

**Everything else writes through the buffer.** The JSON framing, the
per-query separation of section 4.6, and the sorted output of section 5
append to the same buffer, so nothing reaches the stream around it and
the order of bytes is the order of appends. Interactive mode flushes the
buffer before it prompts, so a prompt never appears before the rows of the
query it follows. Diagnostics go to standard error and never touch the
buffer.

**Sorting.** `-s` collects every row as now and then writes the sorted
rows through the same appends. It gains the buffer and nothing else.

**Cost.** About 120 lines in `cmd/relic/output.c`, most of them the integer
renderer and the flush. `chain-closure` writes 2,001,000 rows of about
twelve bytes, 24 MB, which one `fwrite()` per 64 KiB moves in well under
0.1 s; the writer's share of the profile falls from 58 per cent to a few.

---

## 4. Streaming answers

A query whose head keeps every variable its body binds is answered from
its rule's derivations directly. The rule's `EMIT` delivers each tuple to
the caller's callback as it is derived; no answer relation is filled,
hashed or streamed afterwards. Every other query keeps the answer relation
of `specs/design-query.md` section 3 exactly as it is.

### 4.1 Which queries stream

Call a query rule **projection-free** when every variable that occurs in
its body occurs in its head. An anonymous `_` is a body variable the head
does not mention, so a body with one is not projection-free; a constant in
the head, a repeated head variable, a comparison and a negated literal do
not affect the property. The property is read off the query's syntax when
its rule compiles, and the rewrite preserves it: a supplementary predicate
carries every variable of its prefix that the rest of the rule reads, and
a variable the head reads is one of them, so the rewritten query rule
binds the same variables and keeps them all.

**Why its derivations are a set already.** The query rule runs once, after
every component it reads is at its fixed point, over full views. Its plan
enumerates bindings of the body's variables by choosing one tuple per
positive literal in plan order; two different choices for a literal differ
in some column, which is either a constant, which both match, or a
variable, so they differ in a variable's value. The sequence of tuples
chosen is therefore a function of the binding, and the nested enumeration
visits each sequence once, so each full binding is derived exactly once.
The head tuple is a function of the binding, and when the head mentions
every body variable that function is injective: distinct bindings give
distinct head tuples. The derivations of a projection-free query rule are
pairwise distinct, and inserting them into a relation would reject
nothing. A projecting rule has no such guarantee, and keeps the relation.

### 4.2 What the plan does

A plan has one `EMIT`, and its target is a relation. This design makes the
target one of two things, chosen when the query's plan is built:

```c
struct relic_emit_target {
	relic_relation_t	*relation;	/* an answer or a predicate's */
	relic_row_cb		 callback;	/* or a caller to deliver to */
	void			*callback_ctx;
};
```

A component's rules, generated rules and projecting query rules emit to a
relation as now, through the pending batch of
`specs/design-evaluation-cost.md` section 6. A projection-free query rule
emits to the callback: `EMIT` builds the head tuple into the same pending
batch, and the batch is drained by decoding each tuple to a
`relic_value_t` row and calling the callback once per row, in batch order.
A callback that returns nonzero stops the run as a stopped stream stops it
now. The batch stays so that one code path fills a tuple whatever its
target, and the drain is the only place that knows which target it has.

**A streaming run allocates nothing and cannot fail.** The query rule's
plan and its state are allocated in the evaluation arena before it runs,
every index it looks up was built when it was planned, and its `EMIT`
inserts nothing, so neither limit is checked and no allocation is made
between the first row and the last. The only way the run ends early is
the callback's own nonzero return. This is asserted: the evaluation's
arena mark and the account's totals are the same after the run as before
it. The consequence for a caller is the one `relic_query_run()` promises
now, that a stopped stream is the caller's doing, with one refinement: a
streamed answer that was not stopped is complete, and a failure during
the query's evaluation is reported before any row is delivered, since
every failure that can happen happens in the components the query rule
reads, which run first.

**The order is the order the relation would have had.** The answer relation
lists its tuples in insertion order, which is the query rule's derivation
order, and section 4.1 shows no derivation is rejected, so the rows a
streaming run delivers are the rows the relation would have streamed, in
the same order. The golden tests of every format stay byte for byte, and
D-4 is untouched: the derivation order is the plan's, and the plan does not
depend on the target.

### 4.3 What else sees it

* **`relic_query_collect()`** collects through the callback as it does now
  and does not know which target fed it.
* **The counters.** A streamed row is counted as a derivation, as every
  emitted tuple is; it is not an inserted tuple, since nothing was
  inserted, so `tuples inserted` on `chain-closure` falls from 4,002,000 to
  2,001,000 and says what it means. `-v` counts rows from the callback,
  as now.
* **Explain** prints `stream $q0(X, Y)` in place of `emit $q0(X, Y)` for a
  streaming query rule, so a reader sees which queries hold no answer.
* **Accounting.** An answer relation that streams is empty throughout its
  query's run and counts nothing, which `specs/design-query.md` section 6
  already requires outside a run and now holds inside one as well.
* **The busy context.** Rows are delivered while the query's evaluation
  is in progress rather than after it, and the context is busy for the
  whole call either way (`specs/design-api.md` section 6), so a callback
  could never call back into the context and still cannot.

### 4.4 Cost

`chain-closure` loses 2,001,000 hashed inserts and 33.6 MB of live heap,
with the slot generations that table left behind; `tc-sparse-cyclic`
loses 979,104 inserts and 17 MB. Every `p(X, Y)?` over a relation, which
is how the comparison workloads and most interactive use ask for a
relation, holds one copy of it instead of two. About 80 lines in
`lib/eval.c` and `lib/plan.c`.

---

## 5. Subsumption and the free rewrite

### 5.1 A free adornment subsumes the others

**Rule.** Within one rewrite, once the query reaches a predicate `p` with
the adornment that binds no column, every call to `p` in every generated
rule, whatever columns the call binds, is a call to `p$ff`, and no other
adornment of `p` is generated.

`p$ff` holds every tuple of `p` in the program's least model, by the
completeness claim of `specs/design-magic.md` section 9.3 for an adornment
that binds no column, and only such tuples, by its soundness. A call
adorned `bf` reads the tuples of `p` whose first column is the bound
value, which are the tuples of `p$ff` whose first column is that value: a
lookup into `p$ff` on that column, which the planner gives it as it gives
any bound literal. Nothing the bound adornment could hold is missing, and
nothing in it was ever cheaper than what the free copy holds, since the
free copy is computed in full beside it.

The redirected call binds the same variables the adorned call bound, so
the literals after it in the section 5 order see the same bound set and
their adornments do not change. Its magic rule and its supplementary cut
are not generated, since a call to `p$ff` propagates no demand: there is
no magic predicate for a free adornment, per section 4. The
stratification of the rewrite is a guard (section 9.1) and stays one: the
redirection adds positive edges from generated rules to `p$ff` and removes
the edges to `p$bf`, `magic$p$bf` and the cuts, and a negated literal
still names a predicate read completely, so no cycle through negation can
appear.

**Adorn first, draft second.** The rewrite today drafts a rule as it
discovers an adornment, and a bound adornment of `p` may be discovered
before the free one. Rather than drafting and then withdrawing, the
rewrite is split into two passes over the same worklist: the first
computes the set of adornments the query reaches, applying the rule as it
goes, so that reaching `p$ff` collapses every adornment of `p` already
reached and every one reached later; the second drafts rules for the
adornments that survive. The first pass reads each rule's body in the
order `relic_plan_order()` gives it for each head adornment, which is what
the draft would read, and drafts nothing, so it costs no budget. The
predicate limit and budget count the adornments the first pass keeps; the
literal budget counts what the second pass drafts. A rewrite that
outgrows a budget is abandoned as now, in the pass where it outgrows it.

### 5.2 A rewrite with no bound column is no rewrite

**Rule.** When the first pass ends with no adornment that binds a column,
the query reads every predicate it reaches completely, and no rule is
generated.

Section 3 of `specs/design-magic.md` computes what a query reads completely
from the program's structure: facts, negation and their dependencies. This
adds a fourth source: every predicate the first pass adorned free. The
complete set is closed over components as before, phase 1 evaluates the
program's own schedule for those components, and the query rule is planned
over the program's relations exactly as a query against a materialized
program is now (architecture section 39's second mode). Nothing is copied,
nothing is named `$ff`, and the rewrite's arena holds the adornment set and
nothing else.

This generalizes the existing case rather than adding one: the design
already answers a query against a program whose relations hold its fixed
point without a rewrite, because a rewrite would derive what is there.
The rule says the same when the rewrite would derive exactly what full
materialization of the reached components derives, which is what a set of
free adornments means.

When some adornment binds a column, the rewrite proceeds, and a free copy
`p$ff` stays a copy, for the reason section 4 gives: its rules may call a
predicate `q` with a binding, `q` may be reached no other way, and the
demand for `q` is worth propagating. The rule of section 5.1 has already
made sure no such call is to a predicate that is also free.

### 5.3 Explain

The adornment lines of `specs/design-magic.md` section 10 gain two forms:

```text
adorn pointsTo/2 ff
adorn pointsTo/2 bf, subsumed by pointsTo$ff
adorn pointsTo/2 fb, subsumed by pointsTo$ff
read completely: pointsTo/2
```

The first pass prints an `adorn` line for every adornment it reaches, with
the subsumption noted, so a reader sees what the query would have asked
for; and when section 5.2 applies, `read completely` names the predicates
the query reads from the program and the rewrite section ends there,
followed by the plans of the program's components and of the query rule.

### 5.4 Cost

`andersen` runs the program's four rules over one relation: 0.26 s against
0.60 s without output, 138,277 tuples held against 429,560. `cspa` and
every free query over a program whose recursive rules call the recursive
predicate after another literal gain the same. A bound query, `path(a,
X)?`, reaches no free adornment and is unchanged. About 150 lines in
`lib/magic.c`, most of them the split into two passes.

---

## 6. Index keys and tagged tables

### 6.1 A key's values live in the index

`specs/design-index.md` section 3 stores nothing twice: a key is compared
by reading the key columns of its first tuple. That read is a random
access into the column array for every occupied slot a probe visits, and
`gather_key()` walks all `RELIC_MAX_ARITY` columns to collect them. On
`andersen`, `find_key()` is the top symbol of the evaluation's profile
before and after section 5.

The index stores each key's values beside its chain:

```c
struct relic_index {
	relic_idtab_t		 keys;		/* key hash -> key identifier */
	relic_index_key_t	*entries;	/* per key: first, last, frontier */
	relic_dom_t		*key_columns;	/* per key: its values, key_length wide */
	relic_tuple_id_t	*next;
	uint16_t		 key_length;	/* columns in key_mask */
	uint16_t		 key_column[RELIC_MAX_ARITY];	/* the columns, ascending */
	...
};
```

`key_columns[(k - 1) * key_length ..)` holds key *k*'s values in ascending
column order, written when the key is first seen, from the tuple that
introduced it. Comparing a probe against key *k* reads `key_columns`, an
array that a probe's neighbours share cache lines in, and never the
relation; gathering a tuple's key walks `key_column[0 .. key_length)`
rather than 32 mask bits; and the rehash callback reads `key_columns`
sequentially. A key costs `4 * key_length` bytes more, one to a few
columns for every index a plan asks for, accounted by
`relic_index_bytes()` and projected by `relic_index_bytes_after_reserve()`
with the entries. The chains, the frontier and the lookup of section 4 of
the index design are untouched.

### 6.2 A tag beside each slot

`specs/design-memory.md` section 6 keeps a slot at four bytes and
`specs/design-evaluation-cost.md` section 5.2 measured the alternative it
rejected, an eight-byte slot holding the hash, on the canonical index of
the closures: their duplicate checks are almost all successful searches,
which read the matching tuple either way, and the doubled slot array cost
them what the saved reads gave back. Section 5.2 also recorded where the
stored hash pays: a table whose searches are mostly unsuccessful, which
reads an entry per visited slot for nothing, and it took a quarter off
`facts-join`.

This design adds the cheaper form of the same thing, a **tag** of one byte
per slot, to the tables whose searches are unsuccessful or whose entries
are far away, and leaves the canonical index as measured:

```c
struct relic_idtab {
	uint32_t	*slots;
	uint8_t		*tags;		/* NULL for an untagged table */
	size_t		 capacity;
	size_t		 count;
};
```

A tag is the top eight bits of the key's hash; the bottom bits choose the
slot, so a tag is independent of where a key sits. `relic_idprobe_next()`
skips an occupied slot whose tag differs from the probe's without
returning it, so a probe asks its owner about one mismatching slot in 256
instead of every one. A tag array is a fifth of the slot array, not a
doubling, and it is read only when the slot array is, so the successful
search that section 5.2 protects pays one byte more per visited slot and
nothing else.

**Which tables are tagged** is decided per owner when the table is
created, by `relic_idtab_init(table, tagged)`, and the initial assignment
follows the measurement:

| Table | Searches | Tagged |
| --- | --- | --- |
| symbol pool | unsuccessful on every new symbol; entry and bytes far away | yes |
| integer table | as the symbol pool | yes |
| index keys | a miss reads `key_columns` | yes |
| staging sets | unsuccessful on every distinct fact | yes |
| canonical index | successful, per section 5.2 | no, until measured with tags |
| predicate table, rewrite tables | small | no |

The canonical index is measured with a tag under the section 5.2 method
once section 8 is in place, since the peak that measurement weighed is
the one section 8 changes. Whatever the result, the decision is a flag at
one creation site per owner, not a second table type.

**Growth** still asks the owner for each identifier's hash, through the
callback of `specs/design-memory.md` section 6; a tag is not a hash. The
owners that section 7 gives a stored hash answer from it without touching
their bytes, which is what makes their growth cheap; the others recompute
as now.

---

## 7. Symbols and values

### 7.1 The domain identifier carries its type

D-3 chose one 32-bit interned domain for every value and named the
accessors that are its whole interface: intern a value, look one up,
compare two for order, hash a tuple, compare two tuples, format one. This
design changes the encoding behind those accessors and nothing outside
them.

```c
typedef uint32_t relic_dom_t;

#define	RELIC_DOM_TYPE_SHIFT	30
#define	RELIC_DOM_SYMBOL	(UINT32_C(1) << 30)
#define	RELIC_DOM_STRING	(UINT32_C(2) << 30)
#define	RELIC_DOM_INTEGER	(UINT32_C(3) << 30)
#define	RELIC_DOM_INDEX_MASK	((UINT32_C(1) << 30) - 1)
```

A domain identifier's top two bits say its type and its bottom thirty bits
index the table of that type. A symbol value is `RELIC_DOM_SYMBOL | id`
for its symbol identifier, a string value `RELIC_DOM_STRING | id` for the
same identifier, and an integer `RELIC_DOM_INTEGER | k` for its position
in the integer table. Type bits of zero are never assigned, so zero stays
the identifier that is never a value and a zeroed slot stays empty.

**What this deletes.** The value table held one entry and one slot for
every value of every type so that there was one path to a domain
identifier. That path stays one: the symbol pool assigns a symbol's
identifier once, and the value's identifier is a function of it, so two
occurrences of one symbol cannot get two identifiers, which is the hazard
D-3 rejected the tagged 64-bit cell for; that cell had two encodings for
one integer, and this one has exactly one for every value. The value
table becomes the **integer table**, an array of `int64_t` and a tagged
identifier table over it, and a symbol costs it nothing. Interning a
symbol field of an import is one probe into the pool instead of two into
two tables.

**The accessors.** `relic_value_intern()` dispatches on the type: a symbol
or string interns its bytes and tags the result; an integer probes the
integer table. `relic_value_of()`, which returned a pointer to a stored
`relic_value_t`, becomes a decode into a struct the caller provides,
`relic_value_decode(ctx, dom, &value)`, since no struct is stored to point
at; its callers are the row delivery, the comparison, the sort and the
formatters, and each already copies the value it reads. The language and
total orders of `specs/design-memory.md` section 8 compare by type from
the tag and then by the table, as they compare now by the stored type.
`relic_value_t` and `relic_string()` are public and do not change: a
symbol's `v.id` is its symbol identifier as it always was.

**Determinism.** `specs/design-memory.md` section 9 says identifiers are
assigned in first-appearance order and D-3 that their numeric order means
nothing. Both stay true within each type; across types the order is now
the type's, and the thing that made that safe, that nothing may order
values by domain identifier, is the invariant D-3 already asserts in one
place. A tuple's hash folds its identifiers and the probe order changes,
which the seed of `specs/design-hash-seed.md` already makes unobservable.

**Limits.** A context holds at most 2^30 - 1 symbols and 2^30 - 1 distinct
integers, in place of 2^32 - 1 values of every type together. The
diagnostic for a table that is full is the one the pool and the value
table give now, and `RELIC_SYMBOL_MAX` moves to the index mask.

### 7.2 The pool's entry and its bytes

```c
struct relic_symbol {
	const char	*bytes;		/* NUL-terminated, in the string arena */
	uint32_t	 length;	/* bytes before the terminator */
	uint32_t	 hash;		/* of the bytes, under the pool's seed */
};
```

The entry keeps its pointer, so `relic_string()` returns a pointer that
stays valid until the context is destroyed, as `include/relic.h` promises,
and stays sixteen bytes: the `size_t` length becomes a `uint32_t`, and the
hash takes the room. A symbol longer than `UINT32_MAX` bytes is refused
with a diagnostic where the lexer and the import bound a field's length,
which no input this tool reads approaches. The stored hash is what the
pool's rehash callback returns, so growing the pool reads the entries
sequentially and no symbol's bytes; it is also what the tag of section
6.2 is cut from.

**The bytes are packed.** The context arena aligns every allocation to
sixteen bytes, which is right for the structs it holds and wrong for a
pool of strings averaging nine bytes: the scale-10 import spent 52 MB of
arena on 27 MB of bytes. The pool gets a **string arena** of its own,
a `relic_arena_t` initialized with an alignment of one, so a symbol's
bytes and its terminator follow the previous symbol's with no gap; blocks
never move, as arena blocks never do, so the pointer contract holds.
`relic_arena_init()` gains the alignment as a parameter, with
`_Alignof(max_align_t)` for every arena but this one; nothing else about
the primitive changes.

**Per symbol**, at a doubling's worst: sixteen bytes of entry, four of
slot and one of tag at three-quarters load, and the bytes plus one. A
nine-byte symbol costs about 40 bytes at capacity and 30 at load, against
70 now, and no value entry beside it.

### 7.3 A row is hashed once on its way in

The staging set of `specs/design-facts.md` section 4.2 hashes a fact to
make the load a set; the commit's measure step hashes it again for the
membership check against the base store; and the append hashes it twice
more, once for an assertion and once to place it. All four are the same
function of the same columns under the same seed, since every relation
and staging set of one context hashes with the context's seed. The
staging set keeps the hash beside each tuple, four bytes per staged fact
in an array beside the marks, and the measure step and the append take
it from there. `relic_relation_contains_hashed()` and
`relic_base_append_hashed()` are the existing operations with the hash as
a parameter; the unhashed forms compute it and call them, so no caller
that has no hash changes. The assertion in the append compares the given
hash with a recomputed one under `!NDEBUG`, so the debug build still
checks what the release build trusts.

### 7.4 Cost

At scale 10 the import's interning does one tagged probe per symbol
instead of two untagged ones, its growth reads no bytes, and each row is
hashed once instead of four times: the estimate is about half the time
and a third of the memory, from 3.6 s and 351 MB. At scale 1 `facts-join`
lands near 0.15 s against DuckDB's 0.30, and its peak near 20 MB against
SQLite's 9.7; the rest of that gap is the staging set and the file text,
which `TODO.md` records separately. About 250 lines across `lib/value.c`,
`lib/symbol.c`, `lib/arena.c`, `lib/facts.c` and the accessors' callers.

---

## 8. Growth without a residue

An identifier table grows by allocating new slots, rehashing into them and
freeing the old; a column array grows by `reallocarray()`. Section 2
measured that the second leaves nothing behind and the first leaves every
generation resident on macOS, and glibc keeps a freed block below its
dynamic threshold in the heap the same way. The reported peak of
`chain-closure` is up to twice its live heap for that reason.

**Large arrays are mapped.** A growable array whose bytes reach
`RELIC_PAGE_THRESHOLD`, one mebibyte, is allocated with `mmap()` rather
than `malloc()`, grown by mapping the new size, copying and unmapping the
old, and freed by `munmap()`. An unmapped region is returned to the
system at once and leaves the resident set with it, so a growth's cost in
peak is the moment both generations exist, at most one and a half times
the new size, and nothing after it. Below the threshold an array is
malloced as now, where the residue is at most a mebibyte per array and a
mapping would cost a system call and a page's rounding per small table.

The rule lives in `relic_grow_array()` and in the slot allocation of the
identifier table, the two places that allocate anything that grows, so
every column array, slot array, tag array, entry array and `next` array
gets it without knowing. A region records its mapped length beside the
pointer, since `munmap()` needs it and `free()` did not; the growable
array's capacity is that record. `mmap()` and `munmap()` with
`MAP_ANON | MAP_PRIVATE` are BSD interfaces of architecture section 45 on
every platform the tool builds for, and `configure` checks for `MAP_ANON`
against the `MAP_ANONYMOUS` spelling.

**Accounting does not change.** The memory limit charges an array its
capacity in bytes, and a mapped array's capacity is what it charges. The
transient second generation during a growth is not charged, as it is not
now; the limit bounds what is held, not what a copy passes through.

**The slack stays.** An identifier table's capacity is a power of two
because its probe masks the hash, and a column array doubles because one
growth policy serves every arity; a table at the moment after a doubling
is half empty. This design does not change either policy. The residue it
removes was as large as the slack; the slack alone is the remaining gap
between relic's peak and its live heap, and a table that grew by three
halves under a multiply-shift range reduction is the change that would
halve it, recorded in `TODO.md` for measurement rather than made here.

**Cost.** About 60 lines in `lib/util.c` and a `configure` check.
`chain-closure`'s reported peak falls to its live heap, one relation of
33.6 MB after section 4, against the interpreter's 46 MB.

---

## 9. The tests

Every change lands with the test that showed the gap, run against the
tree before it, per `CLAUDE.md`.

* **The writer.** The golden tests of every format under `tests/` hold
  byte for byte, extended with rows that straddle a 64 KiB boundary in the
  middle of a symbol, a string escape and an integer, and with a string
  longer than the buffer. The write-failure tests of `specs/design-cli.md`
  section 7 hold with the failure injected at the first flush, at a later
  one and at the final `fflush()`, each reported once. A unit test of the
  integer renderer covers zero, both extremes of `int64_t` and every
  digit count.
* **Streaming.** For every projection-free query of the corpus, the
  rows delivered are the rows of the same query answered through the
  answer relation, in the same order; the differential harness runs both
  by forcing the relation with a flag of the evaluation, kept for the
  test. The arena mark and the account's totals are asserted equal around
  a streaming run. A query stopped by its callback delivers exactly the
  rows before the stop. `-X stats` on `chain-closure` reports 2,001,000
  inserted tuples.
* **Subsumption.** The central equivalence, `magic(Q, P) ==
  Q(full_materialize(P))`, over the corpus and under every strategy, with
  `andersen`, `cspa` and a program whose free rule calls a bound `q`
  reached no other way added to it. The explain of `pointsTo(X, Y)?` names
  one component member; the explain of a query that keeps a bound `q`
  shows `p$ff` as a copy and `q$bf` with its magic predicate.
* **Index keys.** The index unit tests hold; a test inserts keys whose
  first tuples are later than others' and checks that a lookup reads the
  right chain; the accounting tests charge `key_columns`.
* **Tags.** The identifier table's unit tests run tagged and untagged; a
  test with a hash of all-equal low bits and distinct high bits shows a
  tagged probe asking its owner about one candidate.
* **Values.** The value unit tests hold with decode in place of lookup;
  the limit test fills a table to 2^30 - 1 under a test hook and reads
  the diagnostic; the ordering tests hold across types; a symbol and a
  string of the same bytes intern to two identifiers that compare
  unequal, as now.
* **Hash once.** A debug build asserts the carried hash against a
  recomputed one on every append; the load tests hold.
* **Growth.** A unit test grows an array across the threshold and back
  and checks the capacity and the account; the memory-limit tests hold at
  their golden diagnostics.

The comparison run of `specs/design-bench.md` is repeated at the end with
the four workloads of section 2 and the whole set, and `bench/baseline.txt`
is recorded again.

---

## 10. Order of work

Each step is a commit with its tests and its measurement, in this order,
because each is measured against the one before it:

1. **The row writer** (section 3). No library change. Gate: the closures'
   output share under 5 per cent of the profile.
2. **Streaming answers** (section 4). Gate: `chain-closure` inserts
   2,001,000 tuples and its live heap is one relation.
3. **Subsumption and the free rewrite** (section 5). Gate: `andersen`
   within 10 per cent of `-a`, explain shows one member.
4. **Index keys** (section 6.1) and **tags** (section 6.2) for the index
   keys. Gate: `find_key()` off the top of `andersen`'s profile.
5. **Mapped growth** (section 8). Gate: `chain-closure`'s reported peak
   within 10 per cent of its live heap. Then the canonical index is
   measured tagged, per section 6.2.
6. **Tagged pool, typed identifiers, packed bytes, hash once** (section 7).
   Gate: `facts-join` at scale 10 under 2 s and 120 MB.

Step 1 is the largest win for the least code and blocks nothing; steps 2
and 3 are independent of each other and of 4 to 6; step 5 precedes the
canonical-index measurement that step 4's tag makes possible; step 6 is
last because it touches the most files and is measured on the import,
which none of the others change.

---

## 11. Determinism

* The writer produces the bytes the formats specify, whatever the flush
  boundaries; a boundary is invisible in the output.
* A streamed answer is the answer relation's rows in the relation's order.
* Subsumption is a function of the adornment set, which is a function of
  the program and the query; the explain lines are in worklist order.
* A key's values are stored from the tuple that introduced the key, which
  is the first in identifier order; no lookup result depends on it.
* A tag is cut from the hash, which the seed decides, and decides which
  slots a probe skips and nothing a probe returns.
* A domain identifier's type bits are fixed by the type; its index is the
  table's count at first appearance. Nothing reported depends on either.
* A mapped array holds what a malloced one held; no address is reported.

---

## 12. Amendments this design makes

* `specs/design-cli.md` section 4: the writer's buffer, section 3 here.
  Sections 4.1 to 4.6, 5, 6 and 7 stand.
* `specs/design-query.md` sections 2 and 3: the answer relation is filled
  by projecting queries only; section 4 here. The relation, its lifetime
  and its accounting stand for them.
* `specs/design-magic.md` section 3: a fourth source of the complete set,
  the free adornments of section 5.2 here; section 4: the rule of section
  5.1 here after "An adornment with no bound column has no magic
  predicate"; section 10: the explain lines of section 5.3 here.
* `specs/design-index.md` section 3: `key_columns` and `key_column`,
  section 6.1 here; "Nothing is stored twice" becomes "no tuple is stored
  twice"; section 8: the key columns are accounted.
* `specs/design-memory.md` section 3: the arena's alignment is a creation
  parameter; section 6: the tag array and `relic_idtab_init()`, section
  6.2 here; sections 7 and 8: the entry, the string arena, the typed
  identifier and the integer table, section 7 here; section 9: first
  appearance within a type.
* `specs/design-relations.md` section 3 and `specs/design-facts.md`
  sections 4.2 and 4.3: the mapped growth of section 8 and the carried
  hash of section 7.3.
* `specs/design-evaluation-cost.md` section 5.2: the tag is the cheaper
  form it did not measure; its finding on the canonical index stands
  until section 10 step 5 measures it again.
* `specs/architecture.md` sections 19.2, 22, 33, 43 and 47 gain a
  reference here; D-3's "revisit" is answered by section 7.1, which keeps
  its decision and changes its encoding.

---

## 13. What is deliberately absent

* **A B-tree or a columnar store.** The closures' remaining gap to the
  interpreter is inside the duplicate check that
  `specs/design-evaluation-cost.md` measured and overlapped; it is not
  what section 2 found.
* **Writing without stdio.** A `write(2)` path would save one copy per
  block; it would also take over `-o`, the close and the error contract
  for a copy that costs under a millisecond per megabyte.
* **Streaming a projecting query.** It would need a duplicate check
  without a relation, which is a relation.
* **Merging adornments below the free one.** `p$bf` subsuming `p$bff` is
  the same rule for a larger bound set and a smaller gain; the free
  adornment is the one the comparison workloads and interactive use hit,
  and the two-pass rewrite is the structure a later merge would use.
* **A stored hash in every slot.** Measured and set aside in
  `specs/design-evaluation-cost.md` section 5.2; the tag is a fifth of the
  cost for most of the effect.
* **Growth by three halves.** Recorded for measurement; the residue was
  the larger term.
* **Releasing an import's text or staging set early**, and **a load that
  stages nothing on a parse error**: `TODO.md` items of their own, with
  their own measurements.
