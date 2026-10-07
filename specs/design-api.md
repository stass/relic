# relic library interface design

**Status.** Design document for `librelic` as a library other programs depend
on: the entry points `<relic.h>` exports, the values, facts, queries and
results that cross it, what a callback may do, how a mistake is reported, and
how the binary interface is versioned and audited. Forward-looking, like
every document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/architecture.md` sections 19, 48 to 57, 63 and 65.6;
`specs/design-eval.md` for how a query is answered; `specs/design-magic.md`
for the rewrite a query goes through; `specs/decisions.md` D-5, D-7 and
D-14.

---

## 1. What this document settles

* the shape of the interface: which types are opaque, which complete, and
  what every entry point returns when it fails;
* how a caller builds a symbol or a string to insert;
* what `relic_fact_add()` accepts, what it adds to the program, and what it
  does to what the context has derived;
* what a query's answer is after it has been delivered, and the collector
  built on the streaming callback;
* what a callback may call, and what happens to a call it may not make;
* which symbols the shared library exports, and how that is checked;
* the version-info the library carries, and what changes it;
* the example program and the manual page that ship with the library.

---

## 2. The interface as a whole

The tool is a client of the library. It includes `<relic.h>` and nothing
else of the library's, so the interface this document specifies is the one
the tool uses, extended by the entry points an embedder needs and the tool
does not: programmatic facts and value constructors. Every loader names its
input before taking it, `relic_load_file(ctx, source_name, stream)` as
`relic_load_string(ctx, source_name, text)`.

Three types are opaque -- the context, a prepared query and a collected
result -- and five are complete, because a caller builds or reads them field
by field: `relic_value_t`, `relic_location_t`, `relic_config_t`,
`relic_import_t` and `relic_stats_t` (architecture section 48). A complete
type's layout is part of the binary interface.

`relic_config_t` carries, besides the limits and the switches, `threads`:
the size of the worker pool the context owns, the calling thread included,
at most `RELIC_THREADS_MAX`, a larger count refusing the context rather
than being lowered to it. `relic_stats_t` carries `parallel_rounds`, the
rounds whose work was shared among more than one thread, the one counter
that differs between thread counts (`specs/design-parallel.md` section
11).

Every entry point follows one convention:

| Returns | Success | Failure |
| --- | --- | --- |
| `int` | 0 | -1 |
| a pointer | the object | NULL |
| `void` | -- | nothing to report, or the failure is recorded |

A failure is always explained by at least one RELIC_ERROR diagnostic, and
the last one's text is what `relic_error()` returns. `errno` is not used to
report a semantic error; a failure to open or read a file carries the
system's reason in its message.

---

## 3. Values

A value is a symbol, a string or an integer (architecture section 19). An
integer is written into `v.integer` directly. A symbol or a string is an
identifier of interned bytes, and a caller that has bytes needs the context
to intern them:

```c
int	relic_value_symbol(relic_t *, const char *bytes, size_t length,
	    relic_value_t *);
int	relic_value_string(relic_t *, const char *bytes, size_t length,
	    relic_value_t *);
```

Each fills the value and returns 0, or returns -1 with a diagnostic. The
identifier stays valid for the life of the context, and interning the same
bytes again returns the same one. Every interned value stays for the life
of the context and is counted by the memory limit, so a long-lived context
that names many distinct values grows by them (`specs/design-vocabulary.md`
section 5). `relic_string()` returns NULL for an identifier the context
never assigned, a result's among them; `relic_value_compare()`, which has
no failure to return, reports one with a diagnostic and returns 0, the one
read-only accessor that reports, and does so only when no operation is in
progress; `relic_fact_add()` refuses one with a diagnostic.

**A symbol is what the language can write as one.** Its bytes must match the
lexer's symbol rule, `[a-z][A-Za-z0-9_]*`. A symbol the language cannot
write could still be inserted and derived with, but it could not be queried
for by name, and it would print as text that reads back as something else. A
caller with arbitrary bytes -- spaces, capitals first, NUL bytes, UTF-8 --
uses a string, which holds any bytes at all. The same rule is applied again
wherever a symbol value enters the program, so a caller who fills in a
`relic_value_t` by hand with the identifier of a string's bytes gains
nothing.

`relic_string()` reads the bytes of either back; `relic_value_compare()`
orders any two values of one context.

An aggregate adds no type. A `count` or a `sum` in a row is an integer and a
`min` or a `max` is a value its group holds, so a caller reads an
aggregate's result as it reads any other value,
`specs/design-aggregates.md` section 15.

Nor does a computed value. An integer arithmetic computed is an integer,
and a string a string function computed is a string whose bytes
`relic_string()` returns, interned in the one pool with every other, so a
computed `"a.o"` and a written one are one value; `relic_stats_t`'s
`string_values` counts the strings evaluation computed as well as those the
program wrote, and `storage_bytes` includes the pool,
`specs/design-strings.md` section 14.

---

## 4. Programmatic facts

```c
int	relic_fact_add(relic_t *, const char *predicate, size_t arity,
	    const relic_value_t *values);
```

adds the fact `predicate(values[0], ..., values[arity - 1]).` to the
program, exactly as loading that text would: it stages the one fact and
commits it to its predicate's base store as a load commits its facts
(`specs/design-facts.md` section 4). It is refused, with a diagnostic and
nothing added, when:

* `predicate` does not match `[a-z][A-Za-z0-9_]*`, or is `not`, which the
  grammar reserves for negation where a predicate name would stand;
* `arity` is more than 32, the structural limit of D-7;
* `values` is NULL and `arity` is not zero;
* a value's type is not one of the three, a symbol or string names bytes
  this context never interned, or a symbol's bytes are not a symbol by
  section 3;
* the fact is new and the tuple or memory limit does not admit it, reported
  as a load's refusal is, `limit exceeded: tuples=N while loading p/2`.

Predicates are identified by name and arity together, so a fact of a new
arity creates a new predicate rather than conflicting with an old one.

**The fact is committed as a loaded fact is.** It is in its predicate's
base store when the call returns, needs no check, is counted by `-e` as any
fact of its predicate is, and costs what a loaded fact costs. Programmatic
facts and loaded text can be interleaved in any order, and the program is
the same as the one the equivalent text would build. The commit is the one
path into a base store, so there is nothing for the two to disagree about.

**Adding a new fact discards everything derived** (architecture section
54). The commit discards every derived relation before it adds the fact,
and the next evaluation, whether a query or `relic_materialize()`, evaluates
again from the facts, exactly as it does after a load. A fact the base store
already holds changes nothing and discards nothing. A fact can make a
negated literal false, so nothing derived before it can be kept without
incremental maintenance, which architecture section 70 defers. A discard
with nothing derived to discard returns at once, so a run of calls between
two queries pays for one discard.

### 4.1 Imported facts

```c
int	relic_import_file(relic_t *, const char *source_name, FILE *,
	    const relic_import_t *);
int	relic_import_path(relic_t *, const char *path,
	    const relic_import_t *);
```

add a fact for each record of a csv or tsv table, which `relic_import_t`
describes: the format, whether the first record is a header, the predicate,
and a type for each column (`specs/design-import.md` section 3.2). The
entry points name their input first, as the loaders do. The table's facts
are staged as it is read and committed as a load's are, all or nothing, so
everything this section says of a programmatic fact holds for each imported
one: it needs no check, it is committed through the one path into a base
store, and a new one discards everything derived.

**An unknown flag is refused, where an unknown configuration switch is
ignored.** No switch changes an answer, so a library may ignore one it
predates. An import flag changes what is read: a library that ignored a
header flag it did not know would import the header as a fact. The
description, and the column array it points at, are borrowed for the call
and read once, when it is checked, since caller code runs while the table
is read.

### 4.2 A name's arities

```c
int	relic_predicate_arities(relic_t *, const char *name,
	    uint64_t *arities);
```

sets `*arities` to the arities at which the context knows a predicate named
`name`: bit `n` is set when a statement, a fact, an import or a query has
named `name/n`. It is for a caller that names a relation without its arity,
as the tool's export does (`specs/design-export.md` section 7), and needs
the program's answer to whether the name is ambiguous. Predicates the Magic
rewrite generates and the group predicates of aggregates are the library's
own and are never reported. The call begins with a check, as every
operation that uses the program does, so that every rule loaded has named
its predicates, and fails as that check fails. It is refused, with a
diagnostic, when `name` or `arities` is NULL or the context is busy, and
returns 0, or -1 with `*arities` untouched.

---

## 5. Queries and results

### 5.1 Streaming

`relic_query_run()` computes the whole answer, then passes it to the row
callback one row at a time. A query that fails delivers no rows. The callback
returns nonzero to stop early, which is not a failure.

**An answer is transient.** Once the rows have been delivered, or the
callback has stopped them, the answer relation is emptied. What a context
holds is then the program's relations and nothing on behalf of queries
already answered: a long-lived embedder preparing and running many queries
does not grow by every answer it has computed, and the answers of earlier
queries do not count against `max_tuples` and `max_memory` for the next one.
A caller that wants to keep an answer copies it, which is what the collector
does. Outside a query, `relic_stats()` finds no answers among the tuples
held; from a row callback it counts the answer being delivered.

**A row's values and bytes are borrowed for the call.** A row names its
values by the context's identifiers, and a callback that keeps a value or
its bytes copies the bytes, as it already copies the row: the promise is no
wider than the call, so how long the context keeps a value's bytes stays
the library's to decide (`specs/design-vocabulary.md` section 4).

The group relations of a query's aggregates are transient in the same way:
they are filled when the query runs and emptied with its answer, so they
count against the limits only while the query runs,
`specs/design-aggregates.md` section 10. A `sum` whose total does not fit
an `int64_t` fails the run, with no rows, as an undefined arithmetic result
does, section 4.5 there.

`relic_materialize()` and `relic_query_run()` fail, with no rows, on an
evaluation error: an undefined arithmetic result, a string longer than a
string value can hold, or a pattern that is not a regular expression.
A query whose plan holds an arithmetic step or calls a string function is
gathered before its first row, whatever its head names, since either can
fail on any binding; `specs/design-arithmetic.md` section 9.3 and
`specs/design-strings.md` section 9.3.

### 5.2 The collector

```c
relic_result_t		*relic_query_collect(relic_t *, relic_query_t *);
size_t			 relic_result_rows(const relic_result_t *);
size_t			 relic_result_columns(const relic_result_t *);
const relic_value_t	*relic_result_row(const relic_result_t *, size_t row);
void			 relic_result_destroy(relic_result_t *);
```

The collector is `relic_query_run()` with a callback that copies each row,
and has no other access to the engine (architecture section 55). It returns
NULL, with a diagnostic, when the query fails or the copy runs out of memory,
and never a partial result.

A result holds `rows x columns` values in the order the stream delivered
them. `relic_result_columns()` is the query's number of output columns, even
when there are no rows. `relic_result_row()` returns the row's values, or
NULL when `row` is not less than `relic_result_rows()`; for a query with no
output columns a present row is a non-NULL pointer to no values.

A result is independent of the context's state: it survives later loads,
facts and queries, and may be destroyed and read after the context. It owns
the bytes of its symbols and strings, interned into a pool of its own
under identifiers no context assigns, and reads and orders them through
its own accessors:

```c
const char	*relic_result_string(const relic_result_t *, uint32_t id,
		    size_t *lenp);
int		 relic_result_compare(const relic_result_t *,
		    const relic_value_t *, const relic_value_t *);
```

A result's symbol or string passed to the context -- `relic_string()`,
`relic_value_compare()`, `relic_fact_add()` -- is refused as one it never
assigned, while an integer is its own value in both; a caller that
inserts an answer's symbol or string as a fact builds it from the bytes.
A result that owns its bytes outlives the context and reads nothing of it
(`specs/design-vocabulary.md` section 4).

---

## 6. Callbacks and re-entry

Caller code runs while the library is part way through an operation in four
ways: the diagnostic callback, the explain callback, the row callback, and
the read function of a stream handed to `relic_load_file()` or
`relic_import_file()`, which may be the caller's own (`funopen(3)`,
`fopencookie(3)`). None may start another
operation on the same context, and none can: the refusal is a property of
the context, not of the places it calls out from. Each runs on the thread
that started the operation: the pool's threads run a round's tasks and
nothing else, and whatever a round reports is reported by the calling
thread when the round's merge reaches it.

**A context is busy while an operation runs.** Every entry point that
changes a context -- loading, importing, adding a fact, checking,
materializing, preparing, running or collecting a query, interning a value,
installing a callback, destroying the context -- begins by entering it and
returns only by leaving it, through one pair of functions, and refuses to
enter a busy context:

* a refused call returns -1 or NULL, and a refused destroy or installation
  does nothing;
* it makes "the context was called from one of its own callbacks" what
  `relic_error()` returns, without calling the diagnostic callback, since
  the caller is inside one of its callbacks already;
* it is not counted among the operation's errors, and it lasts only until
  the operation the caller code runs inside returns, when `relic_error()`
  again describes the operation, so a refusal never hides why an operation
  failed or outlives one that succeeded;
* nothing the operation in progress holds is touched.

The busy state covers the whole operation rather than the moments the
library calls out, so a new way for caller code to run -- another callback,
a stream -- is covered without being listed anywhere. Entry points do not
call one another inside the library, since the inner one would be refused;
each has a worker that the others share.

A callback must return to the library. One that leaves by `longjmp(3)` or an
exception leaves its context busy, and every later call on it refused.

A diagnostic's message and location are borrowed for the call, the
location's `file` included: it points into the arena of what was parsed
under the name, which lives as long as the context for a load, as long as
the query for a prepared query and as long as the call for an import
(`specs/design-vocabulary.md` section 2). A callback that keeps a location
copies the name with it.

A refused destroy leaks what it would have released rather than freeing
memory the operation in progress is using. That is the cheaper of the two
failures, and it happens only to a caller that broke the contract.

**Destroying a query reads nothing of its context** unless the query is
being answered, so a caller may destroy the context first, as a binding with
finalizers does in whatever order it likes. A query is marked while it is
answered, and destroying it then -- from its row callback, or any other
caller code that runs while its answer is computed -- is refused; its
context is then certainly alive to record the refusal. Destroying any other
query from caller code is allowed: nothing in a context points into a query
object but the query being answered.

A few entry points only read, and a callback may call them:
`relic_string()`, `relic_error()`, `relic_value_compare()`, `relic_stats()`,
`relic_query_count()`, `relic_version()`, `relic_diag_stderr()` and the
result accessors. They do not enter the context, and nothing they read moves
while an operation is running.

`relic_interrupt()` does not enter the context either: it raises a flag the
operation in progress looks for, and a callback, a signal handler or another
thread may call it while the context is busy. An evaluation that finds it
raised fails, `specs/design-interrupt.md`.

---

## 7. Diagnostics

A library does not write to its caller's standard error uninvited
(architecture section 56). A context starts with no diagnostic callback, and
every note, warning and error then reaches the caller only through
`relic_error()` for the last error. `relic_diag_stderr()` is a ready-made
callback for a caller that wants the tool's format, and is the one the tool
installs. Every diagnostic the library produces, at every level, goes through
the one callback, so a caller that installs one sees all of them and its
standard error sees none.

Every error also has a class, read with `relic_error_class()` beside
`relic_error()`: what kind of failure it was -- input rejected, an
evaluation error, a limit, a file, memory, an interrupt, a malformed call,
or a bug in relic -- so that a program driving the library learns it
without matching the text, which stays free to improve. The classes, the
rule that decides between two, and how every error carries one by
construction are `specs/design-errors.md`.

One diagnostic is produced only on request. An ordering comparison between
values of different types is false and filters its binding out, which is the
language's rule and not a mistake, so the library does not report it by
default. A context created with the `RELIC_F_NOTE_MIXED` switch reports, as a
note located at the comparison, the first such comparison in each rule of the
program, once for the life of the context, and in a query once per answer;
the rules a Magic rewrite generates from a rule, and its semi-naive variants,
count as that rule. The same switch reports an aggregate that has no value
because its group mixes types -- a `sum` over a value that is not an
integer, a `min` or `max` over values of more than one type,
`specs/design-aggregates.md` section 4.4, and an operand of the wrong type
under an arithmetic operator or in a string function's argument, which
leaves its expression without a value and its comparison false,
`specs/design-strings.md` section 4.3. A rule gets one note for
whichever of these it meets first, and the group rule of an aggregate
counts as the rule holding it. Asking for the note allocates nothing, so it
cannot make an evaluation fail. It is a switch in
`relic_config_t.flags` rather than a field, because the structure is frozen
and a library that predates the switch ignores the bit, so a caller that sets
it runs everywhere and loses only the notes. The tool sets it at `-vv`.

---

## 8. Exported symbols

The library is compiled with hidden visibility, and each entry point is
declared with `RELIC_EXPORT`, which makes it visible again. Linking applies
libtool's `-export-symbols-regex '^relic_'` as a second fence. Internal
functions are also named `relic_`, so the fence alone would export them;
visibility is what keeps them in.

The audit holds the two to one list. A test reads the names `<relic.h>`
declares with `RELIC_EXPORT` and the dynamic symbols the built shared library
defines, and requires the sets to be equal: a function
declared but not exported fails a caller at link time, and one exported but
not declared is an interface nobody documented.

---

## 9. Versioning

The library's libtool version-info starts at `1:0:0` when the interface is
frozen. It follows libtool's rules: an added entry point is `c+1:0:a+1`, a
removed or changed one `c+1:0:0`, and a change inside the library alone
`c:r+1:a`. A field added to a complete type changes its layout and is an
incompatible change, which is why the configuration structure is frozen
without a step limit (architecture section 63 has none) and why a counter
added to `relic_stats_t` is one too.

Fields that belong together, such as `threads` and `parallel_rounds`, are
added as one incompatible change rather than two. A change to what an
entry point returns is `c+1:0:0` even when the entry points that read the
new return are added beside it: a result's symbols and strings carrying
the result's identifiers rather than the context's changes what
`relic_result_row()` returns, whatever `relic_result_string()` and
`relic_result_compare()` add (`specs/design-vocabulary.md` section 4).

A new `RELIC_F_` switch is an added interface, `c+1:0:a+1`: the bit is new
and a caller may depend on its effect, while a library without it ignores the
bit, so nothing built against the older interface changes.

`RELIC_VERSION_STRING` and `relic_version()` are the package's release, and
are unrelated to the version-info: the release says what the tool and the
language do, the version-info what a dynamic linker may substitute.

---

## 10. The example and the manual

`examples/embed.c` is the smallest program that does everything section 2
lists an embedder needing: create a context with a limit, install a
diagnostic callback, load text, add facts, stream a query with early
termination, collect one, and read its values. It is compiled and run by
`make installcheck` against the installed header and library, found through
the installed `librelic.pc`, and never against the build tree. That is the
only arrangement that catches a missing export, a header that needs an
internal one, or a `.pc` file that points at the wrong place; `make
distcheck` runs it.

`man/relic.3` documents every entry point, in mdoc, and is checked with
`mandoc -Tlint` where `mandoc` is installed.

---

## 11. What is deliberately absent

* **A step or time limit.** Architecture section 63 defines none; adding one
  to `relic_config_t` later is an incompatible change and bumps the
  version-info. A caller that wants a deadline interrupts the evaluation
  from a timer with `relic_interrupt()`, `specs/design-interrupt.md`. The limits bound what is stored, not what is computed: an
  aggregate's group relation is charged to `max_tuples` and `max_memory`,
  but the join that derives it is bounded by nothing, as any join is not,
  `specs/design-aggregates.md` section 14. Arithmetic and the string
  functions sharpen the question without answering it: a recursion that
  computes a new value per derivation is bounded by `max_memory`, since
  the value table and the pool are charged, but one that computes over
  values it already holds runs until a limit or the caller stops it;
  `specs/design-arithmetic.md` section 13 and `specs/design-strings.md`
  section 13.
* **Sorted results.** A result keeps the stream's order; `relic_value_compare()`
  sorts it, as the tool does for `-s`.
* **Thread safety within a context** (D-14). Contexts share nothing, so
  distinct contexts may be used from distinct threads. The threads a
  context's pool starts are its own and run only inside an operation the
  caller started, so a context is still used by one caller thread at a
  time.
* **Removing a fact.** v1 has no retraction; a caller builds a new context.
* **A value lifetime shorter than the context's.** Interned bytes are never
  released, so a long-lived context grows with the distinct values it has
  ever seen.
