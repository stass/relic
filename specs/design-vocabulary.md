# relic vocabulary design

**Status.** Design document for what a context's tables hold and what
leaves the context: the symbol pool holds values and nothing else, names
live where what wrote them lives, a collected result owns the bytes it
holds, and a row callback's values are borrowed for the call.
Forward-looking, like every document under `specs/`; `TODO.md` records
what is not yet implemented.

**Companions.** `specs/architecture.md` sections 17, 18, 19 and 47;
`specs/design-memory.md` sections 2, 7 and 8, whose tables this design
fills; `specs/design-program.md` sections 3 and 4, whose predicate table
and variables it names; `specs/design-strings.md` section 9.1, which makes
the pool the one path a value's bytes take; `specs/design-api.md`
sections 3, 5 and 6; `specs/decisions.md` D-3, D-24, D-34 and D-35.

---

## 1. What this document settles

Three questions, once, so that the lexer, the compiler, the rewrite and
the public interface answer none of them again:

* what the symbol pool holds, and where a name that is not a value lives
  instead;
* when a constant the text writes becomes an identifier;
* what may leave the context holding an identifier or a pointer, and under
  which contract.

It does not change what a value is, how a tuple stores one, or how the
Magic rewrite derives its rules. Those are read here as given from
`specs/design-memory.md` section 8, `specs/design-relations.md` and
`specs/design-magic.md`. It does not reclaim anything: section 5 says how
the tables grow and why that is left as it is.

---

## 2. The pool holds values and nothing else

The symbol pool is the one path every byte string that is a *value* takes,
per `specs/design-strings.md` section 9.1, and it is the path for nothing
else. **The lexer and the parser intern nothing but a fact's values,
which are interned as the fact is staged.** An identifier token carries
its bytes and length in the text being parsed, a string token its decoded
bytes in the lexer's buffer, and the parser keeps what it needs of them in
the statement's arena:

| What the text names | Where it lives |
| --- | --- |
| a symbol or string constant | the text being parsed, or for a string the statement's scratch, while the statement is parsed; then a rule's or query's is copied into the statement's arena until the check compiles it, and a fact's is interned as the fact is staged |
| an integer constant | the term itself, as a value |
| a variable's spelling | the statement's arena; the compiled rule's variable records the bytes and length for diagnostics and explain |
| a predicate's name | the predicate table's name store (section 3) |
| a function's or an aggregate's name, `not` and `_` | nowhere; the lexer and the parser recognize them by comparing bytes |
| the source name | the arena of what was parsed under it: the program arena for a load, the query's arena for a prepared query, the import's own copy for an import |

A statement's arena is the program arena for a loaded statement, which
lives as long as the context does since statements are never freed
(`specs/design-program.md` section 7), and the query's arena for a prepared
one, released with the query. A variable is resolved within its statement
by comparing bytes: a rule has at most 64 variables (architecture section
63), so the comparison is a short scan and needs no table. The rewrite's
generated variable names, `X1`, `X2` and so on, are string literals of the
library, so they outlive any copy of the rule that names them.

**When a constant becomes an identifier.** A rule's constants and a
query's are interned when the check compiles the statement, and a fact's
values when a load, an import or a fact addition stages them, as
`specs/design-facts.md` section 4.1 has it. Until then a constant is an
integer, or bytes and a length where the table above places them. A
statement that fails before it is compiled or staged interns nothing.

**Source names and the location contract.** A location's `file` is a
pointer into the arena of what was parsed under the name, so it lives as
long as that does: for the life of the context when a load produced it, of
the query when a prepared query did, of the call when an import did. A
diagnostic callback that keeps a location copies it, name included. The
tool copies what it prints and keeps nothing.

What this removes is every path by which text that is not a value reaches
the pool: the pool's count is the number of distinct byte strings the
context has interned as values, and a session's lines, a program's
variable names and a query's source name leave nothing in it.
Architecture section 18's "all symbols and string literals should be
interned" stands, read as the values it was about; its "the evaluator
should almost never compare C strings" stands too, since the parser is not
the evaluator.

---

## 3. The name store

The predicate table keeps its names in a **name store**, an arena of
alignment one like the pool's string arena, which holds each predicate's
name and the source names of the locations the table retains. An entry
keeps a pointer, a length and the hash it was placed by, exactly as a pool
entry does. The table's key is the name's bytes and the arity: the hash is
the bytes' hash under the context's seed with the arity mixed in as one
more word, and the probe compares the arity and then the bytes, which
interning a predicate pays once per statement rather than once per tuple,
at the call site as every identifier-table comparison is. A group
predicate's name and the names of the Magic rewrite's adorned, magic,
supplementary and base-view predicates are rendered into a buffer and
interned as bytes, like a name the text wrote. `relic_pred_t` holds no
symbol identifier; D-24's "a symbol is interned once" is about values, and
a name that is also spelled as a value is stored twice, a predicate's
worth of bytes.

**The table is charged** to `max_memory` as the pool is, whole: its
entries, its index and the store, with the array and the index of the
source names. A growth is admitted first, against what the whole table
would hold after it, and the account is then set to what the table holds,
so a caller naming new predicates without end is refused at the limit
rather than growing memory nothing counts. Charging the store alone would
not do: an entry and its index slot outweigh a short name many times over.
A growth refused fails the statement as any resource does and records
nothing.

**The one location the context retains** past its statement is the
predicate table's record of the first atom reading a predicate,
`referenced_at`, which the undefined-predicate sweep of
`specs/design-program.md` section 6 points at. A query's atom can be that
first reference, and the query's arena dies with the query, so the
function that records the reference, `relic_pred_reference()`, copies the
location's source name into the name store, deduplicated against the
source names already there, once per predicate since only the first
reference is kept. It is the only place a location crosses from a
statement into the context, so it is the only place that copies; a second
such place would copy as well, and a maintainer adding one finds the rule
at the function every retention goes through.

---

## 4. What leaves the context

Three rules say what a caller may hold of the context's state, and for how
long.

**A result owns its bytes.** `relic_query_collect()` copies rows out of the
context, and a result "holds nothing of the context's state": it may be
read after later loads, facts and queries and after the context is
destroyed. So **a result interns the symbols and strings it holds into a
pool of its own** as it collects them, copying each distinct byte string
once, and its symbols' and strings' identifiers are the result's; an
integer is its own value in a result as in a context. A result's identifier
has **bit 31 set**, which no identifier of a context has, so that every
function of the context -- `relic_string()`, `relic_value_compare()`,
`relic_fact_add()` -- refuses a result's symbol or string as one it never
assigned instead of reading some unrelated value of its own: a caller that
passes a result's value where a context's is expected gets NULL or a
diagnostic, never a wrong string. The result has its own accessors:

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
and compares a value whose identifier the result never assigned equal to
everything. A
caller that feeds a result's value back as a fact builds it again from the
bytes with `relic_value_symbol()` or `relic_value_string()`. Its integers
are already values. The collector's pool is the symbol pool's type, owned
by the result, hashing under the context's seed so that an answer cannot
be crafted to crowd it, and charged to nothing, since a result is the
caller's. The collector maps each context identifier to the result's the
first time it copies it, so a byte string repeated across rows is hashed
once.

**The context refuses a value it never assigned.** `relic_string()`
returns NULL for one, `relic_fact_add()` refuses it with a diagnostic, and
`relic_value_compare()`, which cannot return a failure, reports it and
returns 0, so that an embedder sorting with a value the context never
assigned hears about it instead of getting a quietly unsorted answer. The
comparison reports as an operation of its own, entering and leaving the
context as every operation does (`specs/design-api.md` section 6), and
only when no operation is in progress: a comparison made from caller code
the library is running, such as a row callback sorting what it was given,
reports nothing, so that it neither recurses into the diagnostic callback
nor overwrites the error of the operation it runs inside.

Both refusals check an identifier's namespace and range, not its owner: a
value carries no owner, so another result's identifier that this result
has assigned too, or another context's that this context has, reads as
this one's value. The checks keep a mistaken value from reading out of
bounds and catch the one confusion the namespace bit can catch, a result's
value handed to the context; keeping each value with its owner is the
caller's part.

**A row callback's values and bytes are borrowed for the call.** A
streamed row names its values by the context's identifiers, and a caller
that keeps a row past its callback copies the bytes it needs, not the
identifiers, or collects the answer instead. The promise is no wider than
the call so that how long the context keeps a value's bytes stays the
library's to decide. The tool formats each row inside the callback and
keeps nothing; an embedder that wants rows later collects them.

**A location's `file`** lives as long as the arena of what was parsed under
it, section 2; a diagnostic callback that keeps a location copies it, name
included.

---

## 5. The tables grow for the life of the context

The symbol pool, the value tables, the predicate table and the name store
release nothing: every distinct value a load, an import, a fact addition,
a query's constants or an evaluation interns, and every predicate name a
program or a query names or a rewrite generates, stays until the context
is destroyed. For the program that is right, since its facts, rules and
names are what the caller wrote. For a run it is a cost: an embedder
preparing a query per request over distinct keys grows the pool or the
integer table by an entry per distinct key, a rule such as
`s(Y) :- s(X), Y = cat(X, "a")` interns a string per derivation, and each
new adornment the rewrite generates adds its predicates to the table.

The growth is bounded where it matters. `max_memory` charges the pool, the
value table, the name store and the relations whole, so a context that
outgrows the limit is refused at the interning that would pass it, with
the limit named, never silently. The ceilings of `specs/design-memory.md`
sections 7 and 8 and `RELIC_PRED_MAX` bound the identifiers. A long-lived
owner whose queries name many distinct constants bounds the growth by
destroying the context and creating another, which costs the program's
load. The interface documentation states the growth as a property of the
context, so that an embedder plans for it.

---

## 6. Tests

* **Names.** `-e` output of every golden test is byte-identical: names
  rendered into the name store equal the names the text writes.
  Diagnostics naming a variable, a predicate, a source name or a query's
  source name are byte-identical, the undefined-predicate warning for a
  predicate first referenced by a query that has since been destroyed
  among them.
* **A session's lines.** A thousand `<query N>` queries against one program
  leave `relic_stats()`'s `symbol_values` and `string_values` where the
  first left them, in the REPL test and through the library.
* **The predicate table's charge.** A context whose limit admits the
  program refuses a statement naming one more predicate past it, with the
  limit named and nothing recorded; the same statement succeeds when the
  limit is raised. Predicates with names of a byte or two, each read from
  a source of its own, are refused before the table holds more than the
  limit.
* **Results.** A collected result read after a further query, a load, a
  fact addition and the context's destruction gives the same bytes and the
  same order under `relic_result_compare()`, an empty string and one
  holding a NUL among them; a result's value passed to `relic_string()`,
  `relic_value_compare()` and `relic_fact_add()` is refused.
* **The comparison's report.** `relic_value_compare()` on a value the
  context never assigned reports once from an idle context and leaves the
  last error of a failed load in place when called from inside an
  operation's callback.
* **Sanitizers.** `make check-asan` and `make check-ubsan` over the suite:
  a location or a result that reads bytes freed with what they were parsed
  from or collected from fails here first.

---

## 7. Determinism

* Rendered names are a function of the entry and equal the names the text
  would write, so `-e` output and diagnostics are byte-identical.
* A result's identifiers are assigned in the order its rows are delivered,
  which is the same at every thread count, so the same answer collected
  twice holds the same identifiers.
* The name store and the result's pool hash under the context's seed, and
  no identifier, output or charge depends on it, as
  `specs/design-hash-seed.md` has it for every table.

---

## 8. What is deliberately absent

* **Reclaiming what a run interned.** The values a query's constants and an
  evaluation introduce, and the predicates a rewrite generates, could be
  released when the run's derived relations are, by dividing each table at
  a mark between the program's entries and a run's. D-35 records why that
  is not built: it needs a program/run boundary in every table, one
  interning function that discards before it appends, a prologue and an
  epilogue around every run, a query instantiated from a template at each
  run, and narrower public promises, all resting on one invariant about
  the lifetime of every identifier, for growth that `max_memory` already
  bounds and refuses loudly. An explicit compaction the caller invokes
  between operations, rebuilding the tables from what the program and the
  current relations hold, is the remedy should a workload need one.
* **Tombstones, a free list or a reference count in any table.** Nothing
  is un-interned, so none is needed, and `specs/design-memory.md` section
  11 keeps them absent.
* **A query atom over an unknown predicate reading an empty relation
  without an entry.** It would stop a stream of queries naming distinct
  unknown predicates from growing the predicate table, and it would give
  the compiled rule, the dependency graph and the stratifier an atom that
  names no predicate, a case each would have to carry. The program model's
  decision to intern a query's atoms stands, and the predicate table's
  charge bounds the growth.
* **A result that references the context.** It would make a result's life
  the context's and its bytes the context's to move; a result that owns
  its bytes outlives the context.
* **A context-lifetime store for every source name.** It would grow by one
  name per session line; a location lives with what was parsed under its
  name, and only the one location the context retains is copied.
