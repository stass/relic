# Library interface

`librelic` is what other programs embed, and the `relic` tool is its first
client: the tool includes `<relic.h>` and nothing else of the library's. This
document gives the contracts that interface rests on -- what a context is
and when it refuses a call, how long anything handed out lives, how a
failure is reported and classified, what a caller can stop, and how the
binary interface is fenced, audited and versioned. The prototypes and every
constant are in `include/relic.h`, and each entry point is documented in
`man/relic.3`; this document says why they behave as they do. It is the
interface around the whole pipeline: every other part of relic is reached
through it.

Related: [architecture](architecture.md#library-interface),
[errors and exit status](architecture.md#errors-and-exit-status),
[C library and ABI](architecture.md#c-library-and-abi),
[threads](architecture.md#threads), [limits](architecture.md#limits),
[evaluation](design-eval.md), [tables](design-tables.md),
[command line](design-cli.md).

## Model

A caller creates a **context**, which holds one program: its predicates,
facts and rules, the relations evaluation derives, and the tables that
intern every value. It loads text, imports tables or adds facts into the
context, prepares **queries** against it, and runs each query either by
streaming its rows through a callback or by **collecting** them into a
**result** the caller owns. Diagnostics reach the caller through a callback
it installs; the last error is also readable afterwards.

Three types are opaque -- the context, a prepared query and a collected
result -- because the library must be free to change what they hold. Five
are complete -- the value, the location a diagnostic points at, the
configuration, an import's description and the statistics -- because a
caller builds or reads each of them field by field and an accessor per field
buys nothing. The value's and the location's layouts are part of the binary
interface. The other three are passed with their size, so they may grow at
the end without breaking it; [Versioning](#versioning) states the rule.

Every entry point follows one return convention: an `int` is 0 on success
and -1 on failure, a pointer is the object or NULL, and a `void` entry point
either cannot fail or records its failure. A failure is always explained by
at least one error diagnostic. `errno` never carries a semantic error; a
failure to open or read a file carries the system's reason in its message.

## Invariants

* A context runs at most one operation at a time, and caller code the
  library runs during an operation cannot start another on it.
* Nothing a caller holds points into a context's mutable state past the
  call it was handed out in, except a collected result, which owns
  everything it holds.
* Every error carries a class, and the class is never NONE while there is
  error text.
* No diagnostic reaches standard error unless the caller installed a
  callback that writes it there.
* The shared library exports exactly the entry points the header declares.
* Answers, diagnostics and every counter but the one counting shared rounds
  are the same at every thread count, and answers are the same under every
  evaluation strategy.

## Contexts and re-entry

Caller code runs while the library is part way through an operation in
several ways: the diagnostic callback, the explain callback, the row
callback, and the read function of a stream handed to a loader or an
importer, which may be the caller's own (`funopen(3)`, `fopencookie(3)`).
None of them may start another operation on the same context, because the
operation in progress holds the context's state half changed.

**The refusal is a property of the context, not of the places it calls out.**
Every entry point that takes a context begins by entering it and returns only
by leaving it, through one pair of functions, and entering a busy context fails
-- except the few readers listed below, which touch nothing an operation
changes. So a callback may call a reader and nothing else, whatever the entry
point, a lookup such as `relic_predicate_arities()` or `relic_query_loaded()`
included. A list of "places that call out" would be only as good as the list;
marking the context busy for the whole operation covers a new kind of caller
code -- another callback, another kind of stream -- without anyone remembering
to add it. It follows that entry points never call one another inside the
library: the inner one would be refused. Each public entry point is a thin
shell over a worker the others share.

A refused call:

* returns -1 or NULL; a refused destroy or callback installation does
  nothing;
* sets `relic_error()` to a fixed message saying the context was called
  from one of its own callbacks, and `relic_error_class()` to ARGUMENT,
  without calling the diagnostic callback, since the caller is inside one
  of its callbacks already;
* is not counted among the operation's errors, and lasts only until the
  operation the caller code runs inside returns, after which the error
  state describes that operation again -- so a refusal never hides why an
  operation failed or outlives one that succeeded;
* touches nothing the operation in progress holds.

The readers, which a callback may call: the error text
and class, a value's bytes, value comparison, the statistics, the count of
the program's queries, the version, the stock stderr diagnostic callback,
the result accessors, and `relic_interrupt()`. None of them enters the
context, and nothing they read moves while an operation runs. Destroying a
query that is not being answered enters nothing either
([Destroy order](#destroy-order)).

**A callback must return to the library.** One that leaves by `longjmp(3)`
or a C++ exception leaves its context busy for good, and every later call
on it refused. Every callback runs on the thread that started the
operation. The context's pool runs internal evaluation and bulk-import jobs;
workers never invoke caller code. Callbacks run at boundaries where those
jobs cannot mutate the state a callback's context readers observe. Round
diagnostics are reported by the caller when the merge reaches them
([parallel evaluation](design-parallel.md)); import failures follow the
[ordered input path](design-bulk-load.md#scalar-replay-is-the-refusal-path).

## Destroy order

A context and the queries and results prepared from it may be destroyed in
any order, so that a language binding whose finalizers run in an order it
does not choose stays correct.

* **A query reads nothing of its context when destroyed**, unless it is
  being answered. A query owns its answer relation and its compiled rule;
  nothing in the context points into a query object except the query being
  answered, so freeing an idle query touches only its own memory. Destroying
  the query being answered -- from its row callback, or any caller code
  that runs while its answer is computed -- is refused; its context is then
  certainly alive to record the refusal. Destroying any other query from
  caller code is allowed.
* **A result reads nothing of its context at all**, ever, by construction
  (see [Results own their bytes](#results-own-their-bytes)).
* **A refused context destroy leaks** what it would have released rather
  than freeing memory the operation in progress is using. It happens only to
  a caller that broke the contract, and a leak is the cheaper failure.

## Values

A value is a symbol, a string or an integer
([values and types](language.md#values-and-types)). A caller writes an
integer into the value directly. A symbol or a string is an identifier of
interned bytes, so a caller that has bytes asks the context to intern them
through the value constructors. Interning the same bytes again returns the
same identifier, and the identifier is valid for the life of the context.

**A symbol is what the language can write as one.** The bytes given for a
symbol must match the lexer's symbol rule. A symbol the language cannot
write could be inserted and derived with, but it could never be queried for
by name, and it would print as text that reads back as something else. A
caller with arbitrary bytes -- spaces, a capital first, NUL, UTF-8 -- uses a
string, which holds any bytes. The rule is applied again wherever a symbol
value enters the program, so a caller who builds a value by hand with the
identifier of a string's bytes gains nothing.

An aggregate or a computed value adds no type. A `count` or `sum` is an
integer, a `min` or `max` is one of its group's values, and a string a
function computed is interned in the same pool as every other, so a computed
`"a.o"` and a written one are one value.

**Values live as long as the context.** Interned bytes are never released
and are charged to the memory limit, so a long-lived context grows with the
distinct values it has ever seen and is refused at the limit; an embedder
whose queries name unbounded distinct constants replaces the context. Why
nothing is reclaimed is
[growth for the life of the context](design-values.md#growth-for-the-life-of-the-context).

## Facts and imports

`relic_fact_add()` adds one fact exactly as loading its text would: it
stages the fact and commits it to its predicate's base store through the one
commit every load uses ([facts](design-facts.md)), so there is nothing for
two paths into a base store to disagree about. The fact is in the store when
the call returns and needs no check afterwards. A refused fact -- a name the
language cannot write, a value the context did not make, a new fact the
limits do not admit -- adds nothing.

**A new fact discards everything derived.** A fact can make a negated
literal or an aggregate's result false, so relations derived before it
cannot simply be extended; one rule for every program -- discard and
evaluate again from the facts -- keeps the question from arising
([the discard](design-facts.md#the-discard)). A fact the store already holds
changes and discards nothing, and a discard with nothing derived returns at
once, so a run of additions between two queries pays for one discard.

An import adds a fact per record of a csv or tsv table, staged as read and
committed all or nothing, so each imported fact behaves as a programmatic
one does ([tables](design-tables.md)).

**An unknown import flag is refused, where an unknown configuration switch
is ignored.** A configuration switch selects a strategy or asks for a note;
no switch changes an answer, so a library may safely ignore a bit it
predates and a caller that sets it runs everywhere. An import flag changes
what is read: a library that ignored a header flag it did not know would
import the header as a fact. Why the import enumerators start at one is
[design-tables.md](design-tables.md#through-the-library)'s.

`relic_predicate_arities()` answers at which arities a name is a predicate
the program knows, for a caller that names a relation without its arity, as
the tool's export does. Predicates the library generates -- the Magic
rewrite's and the group predicates of aggregates -- are never reported.

## Queries and their answers

`relic_query_run()` delivers a query's answer to a row callback one row at a
time. The callback returns nonzero to stop early, which is not a failure.
No row is delivered until everything in the evaluation that can fail has
succeeded, so a query that fails -- at a limit, on an evaluation error --
delivers nothing; an interrupt is the one exception
([Interruption](#interruption)).

**A query owns its answer.** Its answer is a relation the query object
holds, created and released with the query, not a predicate, so preparing
queries adds nothing to the predicate table; why, and how the answer
relation is named and accounted, is
[the query object and its answer relation](design-eval.md#the-query-object-and-its-answer-relation).
A query changes neither the program nor what a later query answers; the
values, predicates and counts it interns persist for the life of the context
([growth for the life of the context](design-values.md#growth-for-the-life-of-the-context)),
so memory headroom and counters may depend on earlier runs.

**An answer is transient.** It is emptied when its rows have been delivered
or stopped, so a context holds nothing on behalf of queries already
answered: a long-lived embedder does not grow by the answers it has
computed, and earlier answers do not count against the limits of the next.
`relic_stats()` called from a row callback counts the answer being
delivered; called outside a query it finds none. A caller that wants to
keep an answer collects it.

**A row's values and bytes are borrowed for the call.** A streamed row names
its values by the context's identifiers, and a callback that keeps a value
past the call copies its bytes. The promise is no wider than the call so
that how long the context keeps any bytes stays the library's to decide.
The same holds for a diagnostic's message and location.

**The collector** is `relic_query_run()` with a callback that copies each
row, and has no other access to the engine, so it answers exactly what the
stream delivers in the same order. It returns NULL, never a partial result,
when the query fails or the copy runs out of memory. A result knows its
width even with no rows.

## Results own their bytes

A collected result is independent of its context: it survives later loads,
facts and queries, and may be read after the context is destroyed. To make
that true, **a result interns the bytes of its symbols and strings into a
pool of its own** as it collects them, once per distinct byte string, and
its symbol and string identifiers are the result's, read and ordered through
the result's own accessors. Integers are their own values in both. The pool
hashes under the context's seed, so an answer cannot be crafted to crowd it,
and is charged to no limit, since a result belongs to the caller.

**A result's identifiers carry bit 31, which no context identifier has.**
The namespace bit is what keeps the commonest confusion -- a result's value
handed back to the context -- from silently reading some unrelated value of
the context's: the context's byte accessor returns NULL for it, fact
addition refuses it with a diagnostic that says to rebuild the value from
its bytes, and comparison reports it. A caller that feeds an answer back as
a fact builds each symbol or string again from its bytes.

**The check is of namespace and range, not of owner.** A value carries no
owner. Another context's identifier that this context has also assigned, or
another result's that this result has also assigned, reads as this one's
value. The checks keep a mistaken value from reading out of bounds and catch
the one confusion a single bit can catch; keeping each value with its owner
is the caller's part.

## Comparing values

`relic_value_compare()` orders two values of one context in the language's
total order, and `relic_result_compare()` orders two values of one result.
The second exists for callers that sort a collected answer, as the tool's
`-s` does ([sorting](design-cli.md#sorting)).

**The context's comparison reports a value it never assigned, and only when
no operation is in progress.** It has no failure to return, yet an embedder
sorting with a foreign value deserves to hear about it rather than receive a
quietly unsorted answer, so it reports an ARGUMENT error and returns 0. The
report is an operation of its own: it enters the context, so the diagnostic
callback runs with the context busy as every callback does. Called from
caller code the library is running -- a row callback sorting what it was
given -- it reports nothing, so that it neither recurses into the diagnostic
callback nor overwrites the error of the operation it runs inside. A result
has nowhere to report, so its comparison treats a value it does not hold as
equal to everything.

## Configuration: limits, switches and threads

The configuration holds the limits, the switches and the thread count, and
the tool's `-l`, `-X` and `-j` merely parse into it, so a limit exists in
exactly one place.

* **Limits.** A zero tuple, memory or input limit means none; a zero Magic
  limit asks for the default proportional to the program. What each limit
  counts, and that exceeding one is a diagnostic and discards partial
  results, is [Limits](architecture.md#limits). The limits bound what is
  stored, not what is computed: a join that derives nothing new, or a
  recursion over values it already holds, runs until something stops it.
  There is no step or time limit; a caller that wants a deadline
  interrupts the evaluation from a timer or another thread.
* **Switches** either select an alternative evaluation strategy, for the
  differential tests, or ask for a diagnostic; `include/relic.h` lists
  them. No switch changes an answer, though two strategies may hold
  different amounts while they compute one -- every strategy switch is
  such a case -- and so one may meet a limit the other does not. A new
  diagnostic is a switch bit rather than a field because a library that
  predates the bit ignores it, where a field it predates and the caller set
  refuses the context.
* **Threads** is the size of the worker pool the context owns, the calling
  thread included. Zero and one both mean the calling thread alone; a count
  past the header's maximum refuses the context rather than being lowered,
  so a caller never runs with a pool it did not ask for. The pool's threads
  start the first time an evaluation shares a round, run only inside an
  operation the caller started, and never call caller code. A thread that
  cannot be started is not an error: the evaluation uses those that could
  be. A library built without threads accepts the field and runs every
  round on the calling thread, and a child forked between operations may
  use its parent's context on its calling thread alone.

A context is not thread-safe and is used by one caller thread at a time.
Distinct contexts share nothing -- the library holds no global mutable
state ([threads](architecture.md#threads)) -- so distinct contexts may be
used from distinct threads freely.

## Interruption

`relic_interrupt()` asks the operation running on a context to stop. It
raises a flag the context holds and does nothing else: it does not enter the
context, allocates nothing, reports nothing and cannot fail. So it may be
called from a signal handler, from another thread -- how an embedder gives
an evaluation a deadline -- or from a callback the operation runs. The flag
is a lock-free C11 atomic, or a `volatile sig_atomic_t` where the compiler
has none, in which case only a handler or a callback may raise it. The
caller keeps the context alive across the call. A NULL context does
nothing, so a handler installed before its context exists is harmless.

**An interrupt reaches the operation running when it is made.** Entering an
operation lowers the flag, after the busy check, so an interrupt made while
nothing runs -- at the tool's prompt, between two calls -- is forgotten
rather than failing an operation that started after it. The price is the
window between two operations, where an interrupt is lost; a caller that
must not lose one keeps its own record and checks it between calls, as the
tool does.

**Only evaluation heeds an interrupt.** Loading, importing, adding a fact,
checking and preparing take time bounded by their input and run to their
end; interrupting a load part way would leave part of its facts, a state no
other failure of a load leaves. Materializing, running and collecting take
time bounded by nothing the caller sets, so they look for the flag while
they evaluate and before each gathered row they deliver. Where the
evaluator looks, and how it bounds the work between looks, is
[design-eval.md](design-eval.md)'s subject.

**An interrupted evaluation fails as a limit fails it**: -1 or NULL, one
error diagnostic of class INTERRUPTED naming what it was doing, and the
context as any failed evaluation leaves it, with nothing to unwind because
the flag is read only where the evaluator could already fail. **Rows
delivered before the interrupt stay delivered.** It is the one failure that
can follow the first row: a streaming query is interrupted while it
derives, a gathered one while it is delivered. Refusing the interrupt until
the answer was out would make a large answer, or one written to a slow
terminal, impossible to stop, and gathering every answer first would give
up streaming everywhere. The run's -1 tells the caller its rows are a
prefix. A callback that returns nonzero, by contrast, ends the rows with
success: the caller chose where they ended.

## Errors

A failure has two faces: the return value, and at least one error
diagnostic whose last text `relic_error()` returns. The text is for a person
and is free to improve; no message text is a stable interface. A program
driving relic needs to act on a failure -- an embedder running an
untrusted program tells "the program is wrong" from "too big for my limits"
from "out of memory", and a second implementation of the language must
agree on why a program fails -- so every error also has a **class**, read
with `relic_error_class()`.

**A class is what the caller can do about the failure.** The text says
what happened.

| Class | The caller | Covers |
| --- | --- | --- |
| REJECTED | fixes the input | anything the language spells and does not accept |
| EVALUATION | fixes the program or its data | a result the language leaves undefined: overflow, division by zero, an over-long string, a computed pattern that is not a regular expression |
| LIMIT | gives it less, or configures more | a configured limit, or a fixed capacity of a table |
| IO | fixes the file | a source or table that cannot be opened, read or closed |
| MEMORY | has an emergency | the allocator refused and no limit was the cause |
| INTERRUPTED | asked for it | `relic_interrupt()` during the operation |
| ARGUMENT | writes the call differently | anything only C spells that is wrong |
| INTERNAL | reports a bug | an invariant the library checks failed |

The rule settles the cases two classes could claim. A configured limit and a
fixed capacity are both LIMIT, because the answer to both is less input; the
memory limit's refusal is LIMIT and the allocator's is MEMORY. A string too
long in program text is REJECTED, because the text can be fixed, while the
same length reached by `cat` during evaluation is EVALUATION, because the
language says the result has no value. A table field that is not an integer
is REJECTED, and a table that cannot be read is IO. An interrupted
evaluation fails as a limit fails it but is its own class, since the caller
asked for it.

**REJECTED is what the language spells; ARGUMENT is what only C spells.**
Program and query text, a table's fields, a value's bytes, a predicate's
name or its number of arguments are REJECTED when the language does not
accept them, whether they arrived as text or as a parameter, and even when
what is wrong is a size, such as a thirty-third argument: the content can
be fixed. A pointer, an index, an enumerator or a flag, the type field or
identifier of a hand-built value, the context a value or query belongs to,
and the moment a call is made are ARGUMENT when wrong: the call must be
written differently. So `relic_fact_add()` given the name `Edge` is
REJECTED, as `Edge(a).` in text is, and given a NULL name is ARGUMENT. A
call refused because its context is busy is ARGUMENT by the same rule. A
backstop behind a check every path has already made -- a predicate's arity
at interning, say -- is INTERNAL where it fires: reaching it means some path
did not check.

**The class is carried by construction.** One reporting function taking a
level, with the class passed beside it, would rest on a hundred error sites
each remembering to pass the right class. Instead the level and the class
are one argument: notes and warnings are reported through functions that
take no class, and an error only through one that requires it, so no error
can be recorded without one and the compiler finds every site. The funnels
that already exist for a kind of failure -- the limit report, the
out-of-memory report, the parser's error function, the evaluation's failure
report -- pass their class once for all their callers. The one function
that formats every error adds the `internal error:` prefix exactly when the
class is INTERNAL, so an INTERNAL error has the prefix however it was
reported and no other error has it. An out-of-range class, which only a
library bug can pass, is recorded as INTERNAL, so the class is NONE exactly
when there is no error text.

**Last-error semantics.** The class is recorded beside the text, by the one
function that records the text, before the callback is called, so a
callback may read it. Text and class have one lifetime: both are set when an
error is recorded and stand until the next error, and a later success
clears neither, so a caller reads both right after the call that failed.
When one call reports several errors, as the parser does after it
resynchronizes, the last one's class is kept with the last one's text; the
errors of one check are nearly always of one class, and where they are not,
the last is the one that stopped the operation. The caveat is an INTERNAL
error met while compiling one statement, after which the check goes on to
the next and a later REJECTED error can be the last. A caller that must see
every INTERNAL error, as the fuzz targets must, reads the class from inside
the diagnostic callback, where each error's class is current.

The class enumeration is part of the interface: a value is never renumbered
or removed, and a caller treats a class it does not know as a failure it
cannot classify. A warning has no class, since a warning is not a failure.
The tool never reads the class: every failed library call exits 1
([command line](design-cli.md#exit-status)).

## Diagnostics

A library does not write to its caller's standard error uninvited. A
context starts with no diagnostic callback, and every note, warning and
error then reaches the caller only through the last-error accessors.
`relic_diag_stderr()` is a ready-made callback writing the tool's format
([diagnostic format](#diagnostic-format)); the tool's own callback hands
over the rows it has buffered and then calls it. Every diagnostic the
library produces, at every level, goes through the one callback, so a caller
that installs one sees them all and its standard error sees none.

A diagnostic's message and location are borrowed for the call. The
location's file name points into the arena of what was parsed under that
name: it lives as long as the context for a load, as long as the query for
a prepared query, and as long as the call for an import. A callback that
keeps a location copies the name with it.

**One note is produced only on request.** An ordering comparison between values
of different types is false -- the language's rule, not a mistake -- so it is
not reported by default. The mixed-type switch reports, as a note, the first
such comparison, wrong-typed operand or mixed aggregate group in each rule,
once for the life of the context, and in a query once per answer; the rules the
Magic rewrite or semi-naive evaluation derive from a rule, and its aggregates'
group rules, count as that rule. The note on a mixed aggregate names no type,
since which row a fold meets first depends on the evaluation strategy. The note
allocates nothing, so asking for it cannot make an evaluation fail. The tool
sets the switch at `-vv`.

### Diagnostic format

`relic_diag_stderr()` writes each diagnostic as one line on standard error.
It is the only code in the library that writes there, and it runs only when
a caller installed it, so the library itself never writes to standard error
uninvited. The line is a prefix, the level, and the message:

    file:line:column: level: message
    name: level: message

The first form is used when the diagnostic has a location naming a file;
the second when it has none, `name` being the callback's context argument,
which the tool sets to the name it was invoked by. With neither, the line
begins at the level. The level is `note`, `warning` or `error`:

    aggregate-stratify.dl:11:1: error: program is not stratified
    aggregate-stratify.dl:11:25: note: quiet/1 depends negatively on loud/1
    aggregate-unsafe.dl:12:20: warning: variable N occurs only once in this rule
    relic: error: limit exceeded: tuples=20 while deriving pairs/1

A note follows the error or warning it explains, as its own line with its
own location. Every INTERNAL error's text, and no other error's, begins
`internal error: `, so such a line reads `relic: error: internal error: ...`
or the same after a location ([errors](#errors)).

**Every byte is escaped on the way out.** A file name comes from a command
line and a message quotes names and literals from the program, so either can
hold a control character or a terminal escape sequence; printed unexamined,
it could retitle the reader's terminal or forge a diagnostic line of its
own. The formatter writes printable ASCII as itself, a backslash doubled,
and every other byte as `\xHH`, so an escape it produced cannot be confused
with one the text held. Messages are built for this: a string is quoted as
the literal that spells it, as explain writes it, and a name or string too
long for the message is cut with a visible `...` rather than silently
shortened. A caller installing its own callback receives the message
unescaped and escapes it before writing it to a terminal.

## Exported symbols

The library is compiled with hidden visibility, and each entry point is
declared with `RELIC_EXPORT`, which makes it visible again. Linking applies
libtool's `-export-symbols-regex '^relic_'` as a second fence. Internal
functions are also named `relic_`, so the regex alone would export them;
visibility is what keeps them in, and the regex keeps out any visible
symbol not named `relic_`, such as a `compat/` replacement function. A
`relic_` internal exported by mistake passes both fences and is caught by
the audit.

**The audit holds the two fences to one list.** `tests/symbols.sh` reads
the names the header declares with `RELIC_EXPORT` and the dynamic symbols
the built shared library defines, and requires the two sets to be equal. A
function declared but not exported fails an embedder at link time; one
exported but not declared is an interface nobody documented, which the next
release would break without knowing it had one.

## Versioning

The library's libtool version-info is maintained apart from the release
version and follows libtool's rules: an added entry point is `c+1:0:a+1`, a
removed or changed one `c+1:0:0`, and a change inside the library alone
`c:r+1:a`. Three consequences are easy to miss:

* **A field added at the end of a structure passed with its size is a
  compatible addition**, `c+1:0:a+1`. The configuration, an import's
  description and the statistics are each passed with the caller's
  `sizeof`, and the library reads or writes only the bytes both sides know,
  by the rule Linux's `copy_struct_from_user()` applies behind `openat2(2)`
  and `clone3(2)`; `include/relic.h` states it for callers. In short, a
  structure the library reads defaults every field past the caller's size,
  and refuses a nonzero byte past its own structure and a size smaller than
  relic 1.0's: `relic_create_config()` with errno `EINVAL` for the size and
  `E2BIG` for the byte, as `openat2(2)` does, and an import as an argument
  error. The statistics are written for the bytes both know and zeroed
  past them, and `relic_stats()`, which cannot fail, answers a size smaller
  than 1.0's by zeroing the caller's bytes. `relic_struct_copy_in()` and
  `relic_struct_copy_out()` in `lib/api.c` are the one place the rule is
  enforced. A release that adds a field:
  * puts it at the end, with a default of zero, so that a zeroed structure
    still asks for every default;
  * leaves a structure the library reads with no implicit padding past its
    1.0 layout, naming any padding as a reserved field the library requires
    to be zero, since padding a caller left unspecified would be refused by
    an older library or read as a field by a newer one; padding inside the
    1.0 layout is harmless, since every library's structure holds it;
  * leaves the 1.0 sizes in `lib/internal.h` alone, which are the floor a
    caller's size is held to, and adds an assertion beside them that the
    new field begins where the one before it ends, updating the size
    assertion to name it, so that padding anywhere past 1.0 fails the build.
* **A field added to the value or the location is incompatible**, because
  it changes the layout callers allocate and neither is passed with its
  size.
* **A change to what an entry point returns is incompatible** even when the
  entry points that read the new return are added beside it: a result whose
  values carry the result's identifiers rather than the context's changes
  what its rows mean, whatever new accessors read them.
* **A new switch bit is an added interface**, `c+1:0:a+1`: a caller may
  depend on its effect, and a library without it ignores the bit, so
  nothing built against the older interface changes behaviour. A new error
  class or entry point is added the same way.

`RELIC_VERSION_STRING` and `relic_version()` are the package release, and
are unrelated to the version-info: the release says what the tool and the
language do, the version-info what a dynamic linker may substitute.

## The example and installcheck

`examples/embed.c` is the smallest program that does everything an embedder
needs: create a context with a limit, install a diagnostic callback, load
text, add facts, stream a query with early termination, collect one and read
its values. `make installcheck` compiles and runs it against the installed
header and library, found through the installed `librelic.pc`, never
against the build tree. That is the only arrangement that catches a missing
export, a public header that needs a private one, or a `.pc` file that
points at the wrong place. `make distcheck` runs it. Without `pkg-config`
there is nothing to check the installation against, and the target says it
skipped rather than passing quietly.

`man/relic.3` documents every entry point in mdoc and is checked with
`mandoc -Tlint` where `mandoc` is installed.

## Determinism

Identical sequences of calls with identical inputs produce identical
identifiers, answers in identical order, identical diagnostics and
identical classes, at every thread count
([Determinism](architecture.md#determinism)). A class is a function of which
failure happened, which is a function of the program, the limits and the
allocator's answers. A result's identifiers are assigned in the order its
rows are delivered, so the same answer collected twice holds the same
identifiers. Counters other than `parallel_rounds` agree on successful
completion. Statistics observed inside callbacks can reflect different
publication points: a shared round reports notes after merging, while serial
execution reports them at its batch boundaries. An interrupt is the one input
that depends on timing, including when requested from a note callback. It can
leave different retained prefixes and counters, and never causes a published
round to be replayed.

## Alternatives not taken

* **A step or time limit in the configuration.** Adding a field to the
  configuration is an incompatible change, and a deadline needs none: the
  caller interrupts the evaluation from a timer or another thread.
* **A code per message.** A class is coarse on purpose. Numbering messages
  would freeze their wording, which is what the class exists to avoid.
* **A result that references its context.** Its life would be the
  context's and its bytes the context's to move; a result that owns its
  bytes outlives the context and reads nothing of it.
* **Thread safety within a context.** Contexts share nothing, so distinct
  threads use distinct contexts; a caller that shares one context across
  threads serializes its own calls, and parallel evaluation is the
  context's own business, inside one operation.
* **Retracting a fact.** There is no retraction; a caller that needs a
  smaller program builds a new context.
* **A value lifetime shorter than the context's.** The reasons are
  [growth for the life of the context](design-values.md#growth-for-the-life-of-the-context)'s.
