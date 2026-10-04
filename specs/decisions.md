# relic decisions

The decisions the design of relic rests on, each settled once so that no
later design relitigates it. A design document cites a decision by its
number. A decision is never renumbered and never edited to mean something
else: one that a later decision changes says so, and the later one names
what it reverses. `specs/architecture.md` is the target design these
decisions shape, and section 76 of it logs where that design departs from
the original specification.

---

### D-1 Naming

The tool is `relic`, the library `librelic`, the public header `<relic.h>`, the
exported prefix `relic_` and `RELIC_`. Internal symbols with external linkage
also use `relic_` but are hidden; file-local symbols are `static`. Program
files conventionally end in `.dl`. The pkg-config module is `librelic`.

An unrelated cryptographic toolkit also publishes `librelic` and a `relic.h`.
relic installs its header directly as `$(includedir)/relic.h`, so the collision
only appears if both are installed under one prefix. Remedies are
`--program-prefix` and a renamed pkg-config module; neither touches the source
tree. Flagged once, not designed around.

### D-2 Language and dialect

ISO C11, compiled as `-std=gnu11` where available so BSD interfaces stay
visible on macOS, falling back to `-std=c11` and then to the compiler default.
No variable-length arrays, no `alloca`, no nested functions, no statement
expressions. `config.h` is included first in every `.c` file and never by
`include/relic.h`, which stays strict C11 and C++-includable.

### D-3 Tuple and value representation

**Decision: one uniform 32-bit interned domain.** Every value, integers
included, is interned into a single value table. A tuple is a dense
`relic_dom_t[arity]`, four bytes per column. Joins, hashing and duplicate
elimination are integer operations that never inspect a payload; only
comparison operators, sorting and output consult the table.

The three candidates, measured on two representative shapes: a symbol-heavy
graph (1M `edge/2` facts over 100k distinct symbols, the shape of dependency,
call and policy graphs) and an integer-heavy table (1M `size/2` facts with 1M
distinct integers).

| | A: 32-bit interned domain | B: tagged 64-bit cell | C: plain 16-byte value |
| --- | --- | --- | --- |
| Cell size | 4 bytes | 8 bytes | 16 bytes |
| Symbol-heavy tuples | 8 MB | 16 MB | 32 MB |
| Symbol-heavy values | ~3 MB | ~3 MB | ~3 MB |
| Integer-heavy tuples | 8 MB | 16 MB | 32 MB |
| Integer-heavy values | ~28 MB | 0 | 0 |
| Equality / join | `uint32` compare | `uint64` compare | type tag plus union |
| Tuple dedup | `bcmp` over `arity*4` | `bcmp` over `arity*8` | needs canonical padding |
| Hash input | dense words | dense words | struct with padding |
| Ordering `<` on int | table lookup | inline after untag | inline |
| Value table needed | yes, for all types | yes, for strings and boxed ints | yes, for strings |
| Distinct-value ceiling | 2^32 - 1 | 2^61 | none |
| Representation hazard | none | two encodings for one integer | padding and union bytes |

**Why A wins.** Symbol-valued columns dominate every workload in section 4 of
the architecture document; integer-heavy columns are the minority case, and A is
half the memory of B and a quarter of C exactly where the data actually is. A
is also the only option where a tuple is trivially `bcmp`-able and hashable as
a dense word array, which is the single hottest operation in the engine: every
insert does a full-tuple probe, and every join key does a partial one.

**What A costs.** The integer-heavy column is its weak case: roughly 24 bytes
plus a hash slot per *distinct* integer, so the second workload above costs
about 28 MB that B would not spend, plus one hash probe per integer at load
time. Repeated integers cost nothing extra, which is why the penalty lands on
distinct-integer columns specifically. There is also a discipline requirement:
domain identifiers are assigned in first-appearance order, so their numeric
order is meaningless and nothing may sort by them. That is one invariant,
asserted in one place, rather than a class of bug.

**Why not B.** The tag is not the problem; the boxing path is. Integers outside
the inline range must be boxed, so one integer has two possible encodings, and
set semantics require the choice to be canonical everywhere a value is
constructed - the parser, `relic_fact_add`, and any future arithmetic. A
non-canonical encoding does not crash; it silently makes `X = Y` false for
equal integers and quietly changes which tuples a relation contains. That is
exactly the "correctness rests on an enumeration of call sites" shape that
`CLAUDE.md` says to design out. B also doubles tuple memory in the common case
to win in the uncommon one.

**Why not C.** Storing `relic_value_t` directly means a tuple contains an enum,
padding and a union, so the fast paths cannot use `bcmp` or a word-wise hash
without first guaranteeing every padding byte and every unused union byte is
zeroed at every construction site. That is the same enumeration hazard as B,
for four times the memory and a branch per column in the join loop.

**Escape hatch.** `relic_dom_t` and about six accessors are the entire
interface to the representation: intern a value, look one up, compare two for
ordering, hash a tuple, compare two tuples, format one. Switching to B later
means rewriting those accessors and the tuple store, not the planner, the
evaluator, the Magic rewriter or the public API. The decision is reversible at
a bounded, known cost, which is the reason to take the simpler option now.

Revisit if a real workload shows integer-heavy columns dominating memory. The
`-X stats` counters report distinct values by type precisely so that this is a
measurement rather than an argument.

### D-4 Determinism

Identifiers assigned in first-appearance order; a load interns the
predicates of its facts as it reads them, before any rule not yet compiled,
since the rule table holds a program's rules and its facts are stored in base
stores (`specs/design-facts.md`). Relations iterate in insertion order: a
base store in the order the text states its facts, and a derived relation
beginning with the facts seeded from its base store. The undefined-predicate
warning points at the first atom that reads a predicate, and the warnings
follow predicate identifier order. Planner ties break by source position.
Magic predicate names derive from predicate and adornment, never a counter.
No result and no `-e` output depends on a pointer value. The golden test
suite depends on all of this. The hash seed was fixed at compile time under
this decision until D-19 made it random per context.

### D-5 Error model

Public functions return `0` or `-1`, or a pointer or `NULL`. Every failure
emits at least one `RELIC_ERROR` diagnostic through the context callback, and
`relic_error()` returns the last message. `errno` is never overloaded for
semantic errors and is preserved for system-call failures. The tool maps
everything to three exit statuses: 0 success, 1 program or query error, 2 usage.
`<sysexits.h>` may be used internally but does not reach the documented
interface. D-32 gives every error a class beside its text.

### D-6 Memory

Arenas by lifetime: context, program, analysis, schedule, query, rewrite,
evaluation. Arena allocation
returns `NULL` and sets a sticky per-arena failure flag; callers check the
pointer, and each phase confirms the flag at its boundary. Relation tuples and
indexes are not arena allocated: they outlive queries, they shrink, and they
are accounted against a limit, as are the two arenas of D-18. They grow
through `relic_grow_array()`, which gives a large array a mapping of its own
(`specs/design-memory.md`). No `malloc()` per AST node or per tuple. The library never
aborts on allocation failure.

### D-7 Limits

Structural and fixed: arity at most 32, so an index key is a `uint32_t` column
bitmask; at most 64 distinct variables per rule, so a binding frame is a plain
array and a `uint64_t` bound-mask. Configurable and unlimited by default:
tuples, memory (relation and index storage, plus the working state of an
evaluation or a rewrite, per D-18), generated Magic predicates, generated
Magic rules. Exceeding a limit is a diagnostic and a nonzero exit, and discards
partial results.

The two Magic limits differ in what happens when they are not set. The rewrite
is an optimization, so when no limit was asked for, it must not decide whether
a query is answered, and must not generate more than a constant multiple of
the program it rewrites. With neither set, the rewrite has two budgets
proportional to the program -- a floor plus a factor times its derived
predicates, and the same times its size in literals for the body literals
the rewrite generates -- and a rewrite outgrowing one is abandoned with a
warning and the query answered from the whole program. Set, each Magic limit
is a limit like the others. The literal budget has no limit behind it: it is
the invariant that a rewrite nobody has sized stays a constant multiple of
the program, and it bounds the generated rules as well, so they have a limit
and no budget (D-21). Configuring either Magic limit lifts both budgets
(`specs/design-magic.md` section 8).

### D-8 The executor runs a plan, and is written once

The executor runs an ordered array of operations, the set
`specs/design-plan.md` section 3 specifies. Every optimization -- indexes,
access paths, body reordering, semi-naive delta variants, Magic rewriting --
changes what the planner emits, never how the executor runs. Each can be
switched off through `-X` to compare against: `-X no-reorder` keeps the
order positive literals are written in, and `-X no-index` reads every
literal by a scan. This is what lets each optimization arrive without
rewriting the executor, and it is the decision the others rest on.

### D-9 The naive evaluator ships

The naive fixed point stays in the library behind `-X naive` for
the life of the project, about 150 lines. It is the differential oracle. Both
evaluators share the parser, program model and relation store, so a
differential failure isolates the evaluator instead of implicating everything.
The alternative, a separate reference implementation in the test suite, would
duplicate the front end and test the duplicate rather than the product.

### D-10 Deltas are tuple identifier ranges

The tuple store is append-only within an evaluation, so the tuples derived in
one semi-naive iteration are exactly those with identifiers in a half-open
range. A delta needs no separate relation and no copying. The invariant this
rests on - no compaction, no deletion, no renumbering during a fixed point - is
asserted in the relation code and stated in `specs/design-eval.md`, because
future incremental maintenance is the change most likely to break it.

A delta variant reads each member of its component through a view of that
range: the delta literal reads the delta, a member literal written before it
the tuples older than the delta, and one written after it everything up to the
delta's end, so every derivation is made in exactly one variant of one round.
The planner orders the delta literal like any other literal, and a lookup
reaches the start of a delta through a per-key frontier in the relation's own
secondary index, so the delta is never copied and has no index of its own.
Where the placed order puts another literal before the delta, the variant also
keeps the delta-first order, and each round runs whichever of the two plans
its delta's size selects. `specs/design-evaluation-cost.md` sections 3 and 4
give the views, the frontier and the choice between the plans.

### D-11 Comparison semantics

Equality is domain-identifier equality, which is exact because everything is
interned. Ordering of symbols and strings is bytewise, not locale collation.
Cross-type equality is false; cross-type ordering filters the binding out and
is reported once per rule at verbosity 2. `=` is a filter and does not bind
when both of its sides are bound; an equality with one free side and one
bound side binds, per D-26. An integer and a float order by value, per
D-33.

### D-12 Generated names cannot collide

Internally generated predicates contain `$`, which the lexer rejects in an
identifier: `$q0` for a query's rule and answer relation, `path$bf` for an
adorned predicate, `magic$path$bf` for its magic relation. Collision with a
user predicate is impossible by construction rather than by a uniquifying
counter, and the names are still readable in `-e` output. A query's name is
interned nowhere, per D-17; the others are predicates.

### D-13 Build

autoconf 2.69, automake 1.13, libtool 2.4.2 or newer, maintainer side only. A
release tarball builds with a C11 compiler, make and a POSIX shell. One
non-recursive `Makefile.am`. CI builds the tarball on every platform with no
autotools installed, because that is the path users take and it is the one that
breaks silently.

### D-14 Concurrency

A `relic_t` is not thread-safe. Distinct contexts share nothing: there is no
global mutable state, which `tests/globals.sh` enforces by failing when the
built library defines a symbol in a writable section
(`specs/design-memory.md`). A context may own
threads of its own, which run only inside an operation its caller started
(D-25); to the caller it is still used by one thread at a time.

### D-15 Encoding

The lexer is byte-oriented and locale-independent: character classification
uses the project's own tables, never `<ctype.h>` with the locale. Identifiers
are ASCII. String literals hold arbitrary bytes, so UTF-8 passes through
unexamined, and output is byte-transparent.

### D-16 Integer literals

Converted into `int64_t` by one integer rule, `relic_lex_integer()`, which
table import shares; out of range is a parse error naming the literal. A `-`
is never part of the literal token: the lexer converts a magnitude and the
parser joins the sign, per D-26, so `-` can be an operator. `INT64_MIN` is
representable.

### D-17 A query owns its answer

A query's answer is a relation the query object owns, created with the query
and released with it, and the head of a query's rule names no predicate. The
predicate table holds program vocabulary only, so its size -- and the cost of
every pass sized by it -- is a function of the program and not of how many
queries have been prepared. `$q<serial>` remains the query's name in
diagnostics and `-e` output, built from the serial when printed and interned
nowhere. Specified in `specs/design-query.md`.

### D-18 Planning costs what its plans hold

A body is ordered in one pass per state of the bound set, at most 65, rather
than one comparison per step, and the order is unchanged. The evaluation
arena and the rewrite arena, the two of operation lifetime, are charged to
`max_memory` beside relation and index storage; the arenas holding the
program, the schedule and a query are not, because the caller wrote what they
hold and a query may outlive its context. A fact is inserted in its turn
without a plan, so the charged arena holds plans of rules with bodies only
and a fact counts once, as its tuple. A refusal by the limit reports
through the same function an exhaustion does, with the limit's message. So a
limit on memory bounds planning time, and no limit on body length is added.
Reverses the part of D-6 that put every arena outside the limit. Specified in
`specs/design-planning-cost.md`.

### D-19 The hash seed is random per context

Every hash table answers which identifier a key has and is never walked in
slot order, so the seed decides where a key sits and how many probes find
it, and no result, `-e` line, identifier, capacity or memory charge depends
on it. A context draws a 64-bit seed from `getentropy(2)` when created and
every table owner hashes with a copy of it; creation fails when there is no
entropy rather than falling back to a constant. With a compile-time seed a
program author could compute every hash and craft an input that drives a
relation into one probe cluster, making its load quadratic inside every
limit. No configuration field, limit or option is added. Reverses the
compile-time seed D-4 recorded; the rest of D-4 is what determinism rests
on. Specified in
`specs/design-hash-seed.md`.

### D-20 Magic prefixes are derived, not repeated

A magic rule's body is the prefix before its call. Written out for every
call, a body of L demand calls holds the square of L in literals and, when
recursive, the cube in plan operations, and the budgets on rules and
predicates do not notice. The rewrite cuts the prefix before every demand
call after the first into a supplementary predicate over its live variables,
so generated bodies are linear in L, and a rule with one demand call
generates exactly what it did. The cost is a relation per cut, accepted
uniformly rather than behind a threshold. Supplementary predicates are
bounded by the rule budget, one rule each, and not counted by the predicate
limit; D-21 retires that budget, and the literal budget bounds them. A
third budget, on generated literals, bounds what remains -- a rule copied
whole once per adornment -- with no limit behind it. It measures the
program as one adorned copy of it, and counts every rule drafted, dropped
or not, so the arena is bounded by construction; D-21 counts a derived
predicate's facts as the two literals of its base-view rule. Like the
other budgets it is lifted when either Magic limit is configured: the budgets
are proportional to the program, the cost of not rewriting to the data, and
a caller who set a limit has sized the rewrite. Reverses the
part of `specs/design-magic.md` section 13 that left supplementary
predicates out pending a measurement: a rule of 400 demand calls, 4.8 KB of
text, took 2.4 s and 1.69 GB through the rewrite against 0.03 s and 1 MB
without it.
Specified in `specs/design-magic.md` sections 6 and 8.

### D-21 Generated rules have a limit and no budget

The rewrite had a third budget, on the rules it keeps, of 256 + 16 x the
program's rules, facts included. Once facts left the program
(`specs/design-facts.md`), it abandoned rewrites it existed to admit: one
adornment of a single rule of k demand calls keeps a magic rule and a
supplementary rule per call, about 2k rules from one rule of the program,
and a rule of 400 calls over 200 facts, answered through its rewrite while
the facts funded the budget, outgrew a budget of 288. A budget on rules in
proportion to the program's literals would bound nothing the literal budget
does not, to within a factor of two: every rule is charged its body before
it is kept, every one but a seed has a body literal, and a seed not the
query's records the first call of an adorned rule whose draft was charged a
literal, so the rules kept number at most twice the literals drafted plus
the query's seeds, and the literal budget bounds them already. The budget
is retired; `max_magic_rules` stays a configurable limit, and configuring
it lifts both remaining budgets as before.
Specified in `specs/design-magic.md` section 8.

### D-22 A query whose plan derives each row once streams its derivations

A plan in which no generator matches an argument with `_` and whose `EMIT`
reads every slot a generator binds derives each answer exactly once: it
visits each binding once, and the head is injective in the binding. The
planner decides this on the built plan, after any rewrite, rather than on
the query as written. Inserting such derivations into the answer relation
of D-17 rejects nothing and costs a hashed insert and a second copy of
the relation, which on `chain-closure` was 2,001,000 inserts and 33.6 MB
for a relation of the same size. The query rule's `EMIT` delivers to the
caller's callback instead, in derivation order, which is the order the
relation would have listed. Any other plan keeps the relation, since a
set without a relation is a relation.

Under a tuple or memory limit no query streams. The limit's contract is
that a refused query delivers no row, and a streamed row is never stored
to be refused, so under a limit the answer is gathered and bounded as
every relation is. D-17 stands: the query owns its answer relation, and a
streaming query leaves it empty.
Specified in `specs/design-comparison-cost.md` section 4.

### D-23 A free adornment subsumes the others

Once a rewrite reaches a predicate with every column free, that copy holds
the whole predicate, and every bound call to the predicate in any generated
rule reads it by lookup rather than through a bound copy with a magic
predicate of its own: `pointsTo(X, Y)?` over Andersen's four rules made
nine relations in one component and three times the derivations of the
program run whole. The rewrite is split into an adornment pass, which
applies the rule and drafts nothing, and a drafting pass over the
adornments that survive. When none of them binds a column the query reads
what it reaches completely, from the program's own relations, and the
rewrite generates nothing; when some do, a free copy stays a copy so that
its rules propagate demand to predicates reached no other way, as
`specs/design-magic.md` section 4 reasons.
Specified in `specs/design-comparison-cost.md` section 5.

### D-24 The domain identifier carries its type

D-3's uniform 32-bit domain stands; its encoding changes. Two bits of type
and thirty of index make a symbol value its symbol identifier, a string
value the same identifier under another type, and an integer an index into
a table of integers only. The value table that held an entry and a slot
for every symbol beside the pool's own is gone, so a symbol is interned
once, and D-3's canonical-encoding hazard does not return: a symbol's
identifier is assigned in one place and its value is a function of it.
The pool's entry stores the hash it was placed by and its bytes are packed
without alignment, and the identifier table gains a tag byte per slot for
the tables whose searches are mostly unsuccessful. Measured at scale 10 of
`facts-join`, a symbol cost 70 bytes and two probes; the estimate under
this decision is 30 to 40 bytes and one. A context holds at most 2^30 - 1
symbols and 2^30 - 1 distinct integers rather than 2^32 - 1 values; a
float type, D-33, halves the integers' index space and takes the other
half. Specified in `specs/design-comparison-cost.md` sections 6.2 and 7.

### D-25 A context's rounds may be shared among threads it owns

A context owns a worker pool of `threads` threads, the calling thread
included, started the first time an evaluation shares a round and joined
when the context is destroyed. A round that reads nothing it writes except
through a view fixed before it began runs over a frozen store: its first
scans are cut into contiguous pieces, the workers derive and screen their
pieces' tuples, and the calling thread merges the candidates in the order
the serial evaluator would have inserted them. So relations, identifiers,
answers, diagnostics and every counter but `parallel_rounds` are the same
at every thread count, and a limit refuses only in a round run serially.
A round is shared only when its work, predicted from the round before it,
repays the threads. The pool's threads call no caller code and block every
signal; a build without POSIX threads or C11 atomics, or configured
`--disable-threads`, has a pool of the calling thread alone. Specified in
`specs/design-parallel.md`.

### D-26 Arithmetic binds through equality and fails on an undefined result

An integer expression over `+`, `-`, `*`, `/`, `mod`, unary `-` and
parentheses may stand as a comparison operand, and nowhere else. An
equality whose one side is a variable nothing before it binds, and whose
other side is bound, binds that variable; safety is the closure of the
bound set over such equalities, still one set computation. An operand that
is not an integer makes the comparison false, as a cross-type ordering
does; a result that does not fit `int64_t`, or a zero divisor, fails the
evaluation as a limit does, because dropping the binding would, through
negation, derive an affirmative answer the program denies. So a strategy
that evaluates more bindings may fail where one that evaluates fewer
answers, and every strategy that answers gives the answer over the
integers; the equivalences, the Magic fallback and streaming are amended to
say so. A computed integer lives in the frame, is looked up read-only, and
is interned only when a tuple holding it is stored, with its identifier
predicted from the table's count so that a parallel round publishes
nothing before its last phase; the integer table is charged to the memory
limit. Reverses the binding half of D-11 and the lexer's half of D-16.
D-33 makes a float an operand as well. Specified in
`specs/design-arithmetic.md`.

### D-27 An aggregate folds a generated relation from a lower stratum

`count`, `sum(V)`, `min(V)` and `max(V)` are written only as the right side
of an equality, `N = count { body }`, and fold the set of rows the body
denotes over the variables local to it: a variable is local when every
occurrence is inside that aggregate, and shared otherwise. The body is
compiled to a generated group rule whose head is the shared variables and
then the local ones, named `agg$<pred>$<arity>$<rule>$<position>` for a
program rule and `agg$$q0$<position>` for a query, and published with the
rule that reads it or not at all -- except a body that is one atom of
pairwise distinct named variables, which denotes the atom's relation
itself and is folded in place, through an index on the columns its shared
variables stand in, with no group rule and no group predicate. The reader
depends on the predicate it folds through an aggregate edge, which
stratification treats as a negative one, so a group is complete before it
is folded and an aggregate in recursion is rejected. A shared variable
must be bound inside the braces as well as outside. `sum` is exact over
the group in a 128-bit
accumulator and fails the evaluation only when the total does not fit
`int64_t`, so its answer and its failure are functions of the group, not
of the order its rows arrived in; a sum over a symbol and a min or max
over values of two types have no value and are noted at verbosity 2. The
unit test drives the accumulator with 2^20 addends of each extreme in
three orders and checks the states near its 2^95 bound one addition at a
time against the compiler's 128-bit integers, rather than 2^31 addends of
each extreme, which takes minutes under a sanitizer. D-33 adds `total`
and `avg`. Specified in `specs/design-aggregates.md`.

### D-28 String operations are functions in expressions, and relic matches its own patterns

`length(S)`, `cat(A, B)`, `substr(S, I)`, `substr(S, I, N)`, `index(S, T)`,
`match(S, R)` and `str(V)` are calls that stand where a term stands in an
expression, and so only in a comparison; a body literal beginning with
`name(` is an atom or the first operand of a comparison by the token after
its `)`, so no name is reserved. Every function is over bytes and total
over operands of its types: positions count from zero, `substr` keeps the
positions that exist, and `index` and `match` return -1 for none, so a
test and its negation are both comparisons and no builtin literal exists.
An operand of the wrong type makes the comparison false, as a symbol
operand of `+` does; a result longer than a string value can hold, and a
pattern that is not a regular expression, fail the evaluation as an
undefined arithmetic result does, for D-26's reason. `match` is decided by
a matcher in `lib/regex.c` of relic's own, over a dialect of POSIX extended
expressions without counted repetition, classes, backreferences or
captures, compiled to at most three instructions per pattern byte and run
as Thompson's simulation, so that a pattern means the same on every
platform and no pattern costs more than the string times the pattern;
`<regex.h>` was rejected for depending on the platform's engine and its
locale, a vendored engine for its audit size. A computed string lives in
a text slot of the frame, is looked up read-only, and is interned only
when a tuple holding it is stored, on D-26's measure, rewrite and commit
with the pool's identifier predicted from its count; the symbol pool is
charged to the memory limit whole. `str` is the one conversion because it
is the one that cannot fail. D-33 amends this sentence for floats:
`float` cannot fail either, and `int` fails as arithmetic does.
Specified in `specs/design-strings.md`.

### D-29 An interrupt fails the evaluation in progress where it can already fail

`relic_interrupt()` raises a flag the context holds, a lock-free atomic or a
`volatile sig_atomic_t`, so a signal handler, another thread or a callback
may call it; it enters nothing and cannot fail. Entering an operation lowers
the flag, so an interrupt made while no operation runs is forgotten rather
than failing the next one. Only evaluation heeds it, because only
evaluation's time is unbounded by its input: the executor returns at least
every fixed quantum of steps as well as with a full batch, the serial
evaluator reads the flag after every return and a worker abandons its job
when it finds the flag raised, so the round is rerun serially and the
calling thread reports the interrupt, and a gathered answer reads it before
each row. The interrupted operation returns -1 with a diagnostic and leaves
the context as any failed evaluation does. Rows a query delivered before
the interrupt stay delivered: it is the one failure that may follow the
first row, and the run's -1 tells the caller the rows are a prefix. The
tool catches `SIGINT` only while an interactive session at a terminal
answers a line. Specified in `specs/design-interrupt.md`.

### D-30 Syntax reserved for extension

Three forms the lexer rejects are reserved rather than merely absent, so
that a later version, or a language built as a superset of this one, can
give them a meaning without changing what any accepted program means.
Digits followed by a period and a digit begin a floating-point literal,
rejected until the type of D-33 exists, and digits followed by an exponent
are rejected with them, since a writer means them as a number and no float
is spelled without its period; a period after digits followed by anything
else is the statement's terminator. `$` followed by a name is a parameter,
a value supplied from outside the program. `$q0`, the name D-12 and D-17
give a query, has the same shape, so a design that adds parameters must
keep the two apart wherever both can appear, in a plan and in `-e` output,
whether by substituting a parameter before planning or by printing it in a
form no generated name takes. `#` followed by a name is a built-in
constant or a directive, and a file inclusion, if designed, is spelled
`#include`. That design must answer the argument of
`specs/design-import.md` section 3.3 that program text is untrusted and
may not open a path of its choosing, through the constraints
`docs/todo-language.md` lists: a name resolved relative to the including
file, no `..` and no absolute path, and a bounded depth. Nothing else is
reserved: `true`, `false` and `null` stay symbols, and no word becomes a
keyword. D-16's one integer rule is unchanged. Specified in
`specs/design-parser.md` sections 3.1 and 3.5 and `specs/design-floats.md`
section 2.

### D-31 `_` in a negated literal is a column the literal does not read

A negated literal holds exactly when no tuple of its relation agrees with
it on the columns that are not `_`. Each `_` is quantified inside the
negation, `not p(_, _)` holds exactly when `p` is empty, and the literal is
equivalent to negating a helper relation projected onto its named columns.
Such a `_` is a wildcard that takes no slot, as in a positive literal, so
the safety invariant of `specs/design-program.md` section 4.1 is unchanged:
every slot of a rule that passes the check is bound by a positive literal
or an assignment. A named variable of a negated literal must still be
bound, since a name used once is more often a misspelling than an
existential, and `_` in a head, in a comparison or as an aggregated
variable is still rejected, since each would be read. The literal is an
existence probe keyed on its named columns, through the canonical index
when every column is named, a secondary index built on demand when some
are, and the relation's emptiness when none is. Stratification and the
Magic rewrite are untouched: both read the literal's predicate and never
its arguments. Specified in `specs/design-negation.md`.

### D-32 Every error has a class, by construction

Every error carries an `enum relic_error_class`, which
`relic_error_class()` returns beside `relic_error()`'s text: input the
language rejects, an evaluation error, a limit, a file, memory, an
interrupt, a malformed call, or an internal error. A class says what the caller can do
about the failure; the text stays the message for a person, and nothing in
the library's contract depends on its wording. The library reports an
error only through functions that take its class, so no error can be
recorded without one, and an internal error's prefix and class come from
one function. The class is recorded with the text before the callback
runs and has its lifetime, and a call refused because its context was
busy reads as a malformed call. The enumeration is part of the interface:
a value is never renumbered or removed. The tool's exit statuses stay
D-5's and do not follow the class. Amends D-5. Specified in
`specs/design-errors.md`.

### D-33 Floating point, when added, is a fourth type equal only to itself

A float is an IEEE 754 double and a type beside symbol, string and
integer, interned by bit pattern in a table of its own. D-24's three tags
are taken and zero type bits are the identifier that is never a value, so
a float shares tag 3 with the integers and the top bit of the index says
which: the integers keep 2^29 - 1 distinct values, which amends D-24's
limit, the floats get as many, and no other limit moves. One function
makes a float of a double, refusing one that is not finite and turning
negative zero into zero, and the literal, the import, the executor, the
fold and the interface reach the float table only through it, so no NaN or
infinity is ever a value and D-3's one identifier per value holds by
construction. A literal's parse is bounded whatever its length, because
the parser's arenas are outside D-18's limit.

An integer never equals a float, so D-11's equality stands: the proof of
`specs/design-arithmetic.md` section 4.5 that the order of binding does
not matter needs `=` to be the identity of values. An integer and a float
order exactly by their mathematical values, which amends D-11's cross-type
ordering for that one pair. A float is an operand of D-26's arithmetic, an
integer beside it converted to the nearest double, which amends D-26's
rule that an operand that is not an integer makes the comparison false,
and a result that is not finite fails the evaluation as D-26's overflow
does. `sum` is unchanged, and D-27 gains `total` and `avg`: `total` sums
integers and floats exactly and rounds once, so D-27's promise that a fold
is a function of the group's set of rows extends to both. `float` cannot
fail and `int` fails as an overflow does, which amends D-28's closing
sentence.

D-4 stands. Comparisons, conversions, the fold of `total` and relic's own
correctly rounded parser and printer are integer arithmetic on bit
patterns, and no libm function is used. Only `+`, `-`, `*` and `/` run on
the hardware, with `FLT_EVAL_METHOD` 0, no contraction and no fast math,
under the default floating-point environment, which every operation and
every worker's job installs, so D-25's rounds stay the same at every
thread count; caller code that runs inside an operation must leave that
environment as it found it. Of the two objections architecture section 3
records, these mechanisms answer D-4's and the one function D-3's, and
floats stay out of version 1. Specified in `specs/design-floats.md`.
