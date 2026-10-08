# Front end

The front end is the first stage of the pipeline: it turns source text into a
program the rest of relic can plan and run. It covers the lexer and the parser,
the syntax tree they build, error recovery, the all-or-nothing load, the
predicate table and its name store, the numbering of variables into slots, the
safety analysis, the compiled form of a rule and the check that drives
compilation, caches its work and says how it failed. What the text means -- the
lexical rules, the grammar and the safety rule itself -- is defined in [the
language reference](language.md); this document describes the mechanism that
implements it. Facts leave the front end as staged tuples for [the base
stores](design-facts.md); compiled rules go on to
[stratification](design-stratify.md) and [planning](design-planning.md).

Related: [architecture: front end](architecture.md#front-end),
[language](language.md), [facts](design-facts.md),
[stratification](design-stratify.md), [values](design-values.md),
[memory](design-memory.md), [aggregates](design-aggregates.md),
[expressions](design-expressions.md), [tables](design-tables.md).

## Model

```text
bytes -> lexer -> tokens -> parser -> statements + staged facts
                                         |            |
                              load commits facts, then splices statements
                                         |
                   check: compile -> rule table, predicate table, queries
                                         |
                              dependency analysis -> schedule
```

* **The lexer** (`lib/lex.c`) decides what a byte sequence is. It interns
  nothing and reports no syntax mistake; a malformed token comes back
  carrying the reason. The one thing it reports is a refusal to grow its
  string buffer, as out of memory, like any allocation.
* **The parser** (`lib/parse.c`) decides what a token sequence means,
  builds the tree, stages ground facts, and owns every syntax diagnostic and
  all error recovery.
* **The syntax tree** (`lib/ast.h`) transcribes the text. It is never
  modified once spliced in; diagnostics and `-e` read spellings from it.
* **The predicate table** (`lib/pred.c`) interns `(name, arity)` pairs and
  records what derives into each predicate and what reads it.
* **The rule compiler** (`lib/rule.c`) numbers variables, resolves atoms,
  decides safety and builds compiled rules. It owns `relic_check()`.

The lexer/parser split keeps error recovery in one place. A lexer that
reported for itself would have to be told when the parser is skipping
wreckage and wants silence -- a flag threaded between two modules to make
one stop talking. Returning the reason with the token removes the flag.

## Invariants

* The lexer interns nothing and reports no syntax mistake; it reports only
  a refusal to grow its string buffer, as out of memory.
* A statement produces at most one syntax diagnostic.
* A load adds all of its facts and statements to the program, or none.
* The program's statement list is append-only, and every statement is
  accepted or rejected once for the life of the context.
* Predicate identifiers are never reused and table entries never removed;
  the predicate marks are never cleared.
* The symbol pool holds values only. Predicate names, variable spellings
  and source names live elsewhere.
* In a rule that passes the check, every slot is bound before it is read.
* A statement's rules reach a rule table together or not at all: a
  rejected or incomplete statement leaves every rule table as it was.

## Lexing

**Classification is a byte table, never `<ctype.h>`.** `isalpha()` answers
a question about the process locale, and that answer would decide whether
a program parses; relic's must be the same in every locale, on every
platform and for either signedness of `char`. The table records what a byte
may do rather than what it is, since several things hold at once: `a`
begins a symbol, continues an identifier and is a hexadecimal digit. `_`
both begins a variable and continues an identifier, so scanning needs no
special case for it. A byte with no class is either punctuation, matched by
value, or -- every byte above 0x7f and every control byte other than
whitespace -- part of no token: an error outside a string literal,
unexamined data inside one, so UTF-8 passes through without the lexer
knowing what UTF-8 is.

**Reserved syntax is rejected by construction.** `$` and `#` belong to no
class, so the lexer rejects them outside a string and nothing else has to
remember to; [reserved syntax](language.md#reserved-syntax) lists what they
are kept for. The same fact keeps every name the engine generates, all of
which contain `$`, from colliding with a program's
([naming](architecture.md#naming)). `relic_fact_add()` checks a name with
the lexer's own rule, so an embedder cannot name such a predicate either.

**An integer is the one token the lexer looks past**, by at most a period
and a digit or an exponent marker and a digit. The reserved floating-point
form is consumed whole as one error token, so the period in `1.5` is not
taken for the terminator the parser resumes at and the statement reports
once; an identifier byte right after the digits is an error at that byte.
The lexer reads only a magnitude, and the parser joins a `-` to it and
checks the range ([lexical structure](language.md#lexical-structure) gives
the rules). The importer reads a field through the same function,
`relic_lex_integer()`, so a table field and a literal accept exactly the
same integers.

**Words are recognized by position, not reserved.** `not`, aggregate names
and function names are ordinary symbol tokens; the parser reads each as a
keyword only where one token of lookahead shows it is one, and refuses `not`
as a predicate name in both places one could be written ([lexical
structure](language.md#lexical-structure) says why). Likewise a body literal
beginning `name(` is an atom or a call according to the token after its
closing parenthesis; its arguments are parsed as expressions either way and
an atom takes each one-node argument as its term, so there is no
backtracking.

**The input is a byte range.** The lexer reads nothing past its end, so a
caller passes text with no terminator, and a NUL inside the range is an
ordinary byte: data inside a string, an unexpected character outside one. A
stream is read whole into one buffer before parsing, so every token is a
pointer and length into contiguous text with no buffer to refill beneath it;
an incremental reader would bound only the text, since the load's staging
grows with the input until the commit regardless. The buffer is freed before
the commit, since nothing the parse kept points into it.

**Locations** give a source name and the line and column of a token's first
byte, from one. Columns count bytes, a tab being one: the number exists to
find a byte, and anything else would make the lexer know an encoding and a
tab stop. The source name is copied into the arena of what is parsed -- the
program arena for a load, the query's arena for a prepared query -- so a
location lives exactly as long as what was parsed under it.

## The syntax tree

The tree is [the grammar](language.md#grammar) as data: terms, atoms,
literals (positive atom, negated atom, comparison, aggregate), expressions,
statements. **A fact is a rule with an empty body**; a third statement kind
or an `is_fact` flag would be a second way to say what the body length says.

**The tree holds rules and queries, not ground facts.** The parser reads a
head's terms into a fixed scratch array; when the terminator shows a fact
whose terms are all constants, it interns the values and the predicate and
adds the tuple to the load's staging, building no statement. A fact with a
variable cannot be a tuple, so it is kept as a statement and the check
rejects it with the diagnostic an unsafe fact receives. Query text stages
nothing; a fact there is reported as a fact where a query was expected.

**A constant in a rule's or query's tree is written, not interned**: an
integer as its value, a symbol or string as bytes copied into the
statement's arena. It becomes a domain identifier when the check compiles
the statement, so a rule or query that fails before then interns nothing.
A ground fact is the exception, since it never becomes a tree: its values
and its predicate are interned as it is staged, while the text is read.

**Anonymous variables are marked, not recognized by name.** Every `_` is
distinct and `_Unused` is an ordinary variable, so the distinction is a
flag set once rather than a string comparison repeated wherever variables
are examined. **An aggregate literal holds its group body**, an array of
literals none of which is an aggregate; which variables it shares with the
rule is the compiler's question. **Expressions keep the shape the writer
gave them**, because `-e` prints rules from them; heads and atom arguments
hold terms only.

Statements form a `TAILQ` because the program arena cannot resize a block
and a growable array would leave every superseded generation behind. Atom
terms and body literals are arrays: their lengths are known when copied and
later stages index them.

## What is interned, and where names live

| What the text names | Where it lives |
| --- | --- |
| a fact's symbol or string | the symbol pool, interned as the fact is staged |
| a rule's or query's symbol or string | the statement's arena, until the check interns it |
| an integer | the term itself, as a value |
| a variable's spelling | the statement's arena; never interned |
| a predicate's name | the predicate table's name store |
| `not`, `_`, function and aggregate names | nowhere; matched by bytes |
| a source name | the arena of what was parsed under it, or the import's own copy |

A statement's arena is the program arena for a loaded statement and the
query's arena for a prepared one. Variables are resolved within a statement
by comparing bytes; at most 64 per rule makes a short scan enough. The
result is that the pool's count is the number of distinct values the
context holds: a session's lines, variable names and source names leave
nothing in it ([values](design-values.md)). A location therefore lives as
long as what was parsed under its name -- the context for a load, the query
for a prepared query, the call for an import -- and a diagnostic callback
that keeps one copies it, name included.

**The name store** is an arena of alignment one owned by the predicate
table, holding every predicate's name -- written, generated by the rewrite,
or a group predicate -- and the source names of retained locations. The key
is the name's bytes and the arity: the seeded hash of the bytes with the
arity mixed in as one more word, and a probe that compares arity, then
bytes. Generated names are rendered into a buffer and interned as bytes. A
name that is also a symbol value is stored twice, a name's worth of bytes.
The whole table -- entries, index, store and the source-name array and
index -- is charged to `max_memory`: a growth is admitted against what the
table would hold after it, and the account then set to what it holds, so a
caller naming new predicates without end is refused at the limit. Charging
the store alone would not do, since an entry and its index slot outweigh a
short name many times over.

**One location crosses into the context**: the first atom that reads each
predicate, which the undefined-predicate warning points at. That atom may
be a prepared query's, whose arena dies with it, so `relic_pred_reference()`
copies the source name into the name store, deduplicated. It is the only
function through which a location is retained, so it is the only place
that copies, and whoever adds a second retention finds the rule there.

## Error recovery

**One message per statement.** After reporting, the parser discards tokens
up to and including a terminator, `.` or `?`, silently. The first mistake
in a statement is the one worth reading; the rest is the parser's
confusion. Messages then count broken statements, and a line of binary is
one message, since lexical errors reach the same reporting function. A
mistake inside a group body is recovered with its statement, because the
grammar never puts a terminator inside braces.

A message that names what was expected also names what was found:
"expected ','" alone leaves the reader to find a missing parenthesis two
lines up. A found byte is quoted as a character when printable and not a
backslash, in hex otherwise, and quoted source text is truncated, since a
message needs only enough of it to recognize which. This is for
readability; how every line is escaped on the way out is in
[diagnostic format](design-api.md#diagnostic-format).

**There is no fourth way to fail.** A parse function returns failure only
after the load is certain to fail, in one of three ways: a mistake in the
source was reported and counted; a module that refused something --
interning, staging, the lexer's scratch buffer -- reported it and parsing
stopped; or an allocation was refused unreported, which marks the arena.
The last is the one a call site could forget, so every parser allocation
goes through one function that marks the arena on every refusal, its own
included, and the load boundary reports an unreported refusal once. A
function that returned failure having done none of the three would drop a
statement and report success, the one outcome worse than an error.

## Loading

The three loaders, `relic_load_string()`, `relic_load_file()` and
`relic_load_path()`, append to one logical program, which is what lets facts
and rules be split across files.

**A load is all or nothing.** Statements go onto the load's own list and
facts into its staging. When the whole input has parsed with nothing
reported, the load commits its facts to their base stores, all or nothing,
and only then splices its statements in. The commit measures and reserves
before it adds anything ([the commit](design-facts.md#the-commit)), so a
limit refuses a load with no store grown and no statement spliced. A
caller that ignores the return value never holds half a file, and
`relic -c` reports every mistake without a later stage wondering which
statements survived. A single reported statement fails the load, because a
program missing a rule is not a smaller program but a different one.
`relic_fact_add()` and table imports stage and commit through the same
functions: there is one road into a base store.

**Arena exhaustion is caught at the load boundary.** Parsing checks its
allocations as ordinary C does, and the arena's sticky flag lets a
half-built statement stop without an error path through every level. The
load asks the arena before it commits, so a missed check cannot become a
truncated program, and clears the flag once reported: the load is the phase
the flag belongs to, and leaving it set would make one transient exhaustion
refuse every later load.

**A failed load keeps** its arena memory, the values its staged facts interned
and the predicates they named ([growth for
life](design-values.md#growth-for-the-life-of-the-context)). An arena has no
per-allocation free, so loading a broken file repeatedly grows memory slowly,
bounded by the text the caller chose to load. A predicate a failed load named
holds no fact and is referenced by nothing, so no warning names it.

## Predicate identity

A predicate is `(name, arity)` and nothing else; `foo/1` and `foo/2` are
unrelated, so there is no arity-mismatch diagnostic. The table is
context-lifetime state, not derived state: entries are never removed and
identifiers never reused, which makes an identifier a stable index for
every later stage and for the base stores, which outlive any query. The
identifier ceiling is checked rather than assumed, since a wrapped
identifier would silently merge two predicates.

Each entry carries marks, each set by the operation it records and never
cleared (`lib/program.h` has the fields):

* **head-named**: a rule's head names it. Set as soon as the head is
  resolved, before safety is known, so an unsafe rule draws one complaint
  and not also "never defined". The rewrite and the aggregate compiler set
  it on what they generate.
* **derived**: a rule with it as head has been appended to a rule table,
  the program's or a rewrite's. Both append through one function whose
  last, infallible step stores the rule, counts it and sets the mark; a
  query sets it on its group rules' heads when it publishes them. The mark
  selects the relation the predicate is read and written through
  ([the funnel](design-facts.md#the-read-and-write-funnel)). It outlives a
  rewrite's table, which is freed after its query; that is harmless,
  because a predicate only a rewrite derives is one the rewrite generated,
  which nothing but a rewrite reads.
* **views**: for the base predicate `p$base`, the predicate whose base
  store it reads. Only the Magic rewrite interns base predicates.
* **kind** and **stands for**: whether the program, the rewrite or an
  aggregate made it, and what a generated predicate is part of. A
  diagnostic names a generated predicate by what the reader wrote and can
  change.
* **program rules**: how many of the program's rules derive it -- the
  position the next takes, which names that rule's group predicates. Rules
  are only appended, so positions, and names, never shift.
* **referenced**: an atom of a rule body or query names it, whether or not
  the statement is accepted; the first such atom's location is kept.

A predicate is **defined** when it is head-named, derived, a base predicate,
or its base store holds facts. Defined is computed, not a mark, so storing
a fact has no mark to remember. The undefined-predicate warning names a
predicate referenced and not defined; one interned only by a failed load or
an abandoned rewrite is referenced by nothing. Every atom is interned, a
query's included, so the warning catches a typo in a query.

## Variables and slots

Compilation gives each distinct variable of a rule a **slot**, an index
into the binding frame, so the evaluator never sees a name. Slots follow
first occurrence, head before body: `path(X, Z) :- edge(X, Y), path(Y, Z).`
numbers `X` 0, `Z` 1, `Y` 2, an order a reader can reconstruct. One
spelling shares a slot; occurrences of `_` never do.

**`_` takes a slot only where it is illegal.** An argument's position
decides what it needs: **binding** (a positive relational literal) binds a
named variable; **probed** (a negated literal) reads a named variable,
which must be bound, and does not read `_` at all
([negation](language.md#negation)); **required** (head, comparison) reads
every argument, `_` included. In the first two positions `_` is a
**wildcard** with no slot. That is its meaning, not an optimization: a slot
would be a frame entry nothing reads, counting against the limit, and a
rule scanning wide relations through `_` would exceed it for no visible
reason. In a required position `_` takes a slot so that the one safety
computation reports it in textual order with the named variables; the rule
is always rejected, so those slots never reach the evaluator.

A variable all of whose occurrences lie inside one aggregate's braces is
local to it: it takes no slot of the rule and is numbered in the group
rule. Every other variable takes its slot at its first occurrence, inside
the braces or not, so wide aggregate bodies do not spend the rule's slots.

**At most 64 distinct variables per rule**, so a frame is a plain array and
the bound set one 64-bit mask. It is a hard limit, not a fallback to a
dynamic bitmap: generous for Datalog, and a fixed word keeps the join loop
free of allocation. The error points at the occurrence that would take the
65th slot.

The resulting invariant: **in a rule that passes the check, every slot is
bound** by a positive relational literal, an assignment or an aggregate's
result, so the evaluator has no unbound-slot case.

## Safety

The rule is [defined in the language reference](language.md#safety); the
check implements it as one set computation over slot masks, not a search.

Let *B* be the slots in arguments of positive relational literals, closed
under assignment: an equality with a bare variable on one side and only
slots of *B* on the other adds the variable, and an aggregate adds its
result once every slot it shares with the rule is in *B*. Let *R* be the
slots in the head, a negated literal or a comparison, plus every
aggregate's shared slots and its result slot, which is compiled as a
required term. Wildcards are in neither. The rule is safe
exactly when `R \ B` is empty, and the offenders are exactly `R & ~B`: a
few ors, a monotone closure whose result does not depend on the order
equalities are tried, and one and-not.

**No reordering search is needed**: positive literals first, then the
assignments in the order the closure fired them, then every other test is
always a valid order when any order is, so bindability is closure
membership ([safety](language.md#safety) states the rule). The same
argument lets the [planner](design-planning.md) place every filter at its
earliest legal point without search. A group body is
checked as the rule it compiles to, whose head names every shared and
local variable, so a shared variable must be bound inside the braces as
well as outside.

**Offenders are reported in written order, by construction.** A variable in
`R \ B` occurs in no positive relational literal, so its first occurrence
is the one its message points at; slots follow first occurrence, so slot
order is textual order and nothing is sorted. Every unsafe variable is
reported, since each is an independent mistake rather than a cascade, and
compilation moves on so one run reports everything wrong with the program.

## Compiling a statement

The compiled rule exists so that no later stage re-resolves anything:
atoms carry predicate identifiers; arguments are a domain identifier, a
slot or a wildcard; expressions are trees over constants and slots that
record their height and node count, which bound every walk and are what the
Magic limit charges. An atom stores no arity, since its predicate has one
and a copy is one more thing to keep in step. A variable record keeps the
spelling, first location and mention count for diagnostics, `-e` and the
singleton warning. A `match` with a constant pattern compiles the pattern
with the rule, and an invalid one rejects the statement at the literal
([expressions](design-expressions.md)).

**A statement is compiled in one pass.** Every length the build needs comes
from the tree except the variable count, which grows in a fixed array of
64. Safety is decided once the body is read, and an unsafe statement is
discarded, not appended. Analysing first and building second would save the
arena memory of a rejected statement, but the build would look each
variable up again by name -- a lookup that could fail only for a reason no
reader could name, since the analysis just assigned every slot. An
unreachable branch whose unreachability rests on an argument about an
earlier pass is what this design avoids; the single pass costs a rejected
statement's memory, once, on a program that is never evaluated.

**A statement holding aggregates is compiled in two passes and appended
all at once.** Which variables are local to a group body depends on text
that may come later, so a first pass classifies occurrences and the second
builds. The statement yields its outer rule and a group rule per aggregate
(or a fold in place; see [aggregates](design-aggregates.md)). Room for all
is reserved at once, the one fallible step, and they are appended group
rules first, outer rule last, so a failed statement leaves the table
untouched.

The rule table is a growable array, not an arena block, because rules must
be reachable by index once the dependency graph exists. Variable-length
parts of a rule live in the program arena as long as its statement.

**Queries are compiled by the same code** into a rule whose head names no
predicate and whose arguments are the query's named variables in
first-occurrence order, so "every output column is bound" is the head
check. Its tuples go to a relation the query owns, so a query adds nothing
to the program but the predicates its body names. A query in a loaded file
is compiled by the check and invalidates the program when unsafe; a
prepared query is compiled into its own arena after the program passes.
[Planning](design-planning.md) turns the rule into a plan.

## The check

`relic_check()` validates the program, reporting every mistake, and every
operation that uses the program runs it first.

**Compilation is incremental, which makes caching trivial.** The statement
list is append-only, so the program keeps a cursor -- the last statement
compiled -- and a check compiles from there to the end. The cursor is a
pointer rather than a count so that resuming costs nothing proportional to
the work already done, and it cannot dangle, since statements live in the
program arena for the life of the context. With no flag of its own, a check with
nothing new compiles nothing and says nothing, each statement is compiled
once for the life of the context, and each diagnostic is emitted once.

**A rejected program stays rejected.** An unsafe rule stays in the tree and
no later load can repair it, so a sticky flag makes every later check
return -1. A check that fails on the flag having reported nothing new says
that the program's errors were reported when it was checked, so the
failure explains itself and the error class reads as a rejected program. A
silent failure with the flag clear has no such reason and is reported as an
internal error.

**Failure has two kinds, and every step says which.** Compiling a statement
returns one of three outcomes, and every step that can fail returns it:

* **accepted**: compiled and appended. The cursor moves past it.
* **rejected**: wrong and reported. The flag is set, the cursor moves past
  it, and compilation continues.
* **incomplete**: memory or predicate identifiers ran out, and nothing was
  learned. The cursor stays, the flag stays clear, the check stops, and a
  later check resumes at the same statement.

The reason travels with the failure rather than being inferred. Inferring
it -- "a step that failed without setting the flag must have run out of
memory" -- would rest the cursor's correctness on a list of which site sets
which flag, and one site that neither reports nor marks would refuse the
program silently and forever, the cursor stuck on a statement it could not
classify.

After compilation the check runs the [dependency
analysis](design-stratify.md#when-the-analysis-runs) and then the
undefined-predicate sweep; both are skipped when compilation stopped early.

**The sweep is not incremental, because its answer is not.** A predicate
referenced by one load may be defined by a later one, so the sweep walks
the whole table at the end of every complete check, skipping predicates it
has reported. A caller that checks between loads may be warned about a
predicate a later load defines -- the price of a program-wide question
without an end-of-program event an embedder does not have. Preparing a
query runs the sweep after the program has passed, so it speaks only of
predicates the query introduced, and runs it for a refused query too,
since the typo may explain the refusal.

The program arena's failure flag is reported and cleared at the check's
boundary, as at a load's.

## Diagnostics

The messages and their wording are under
[diagnostics](language.md#diagnostics); the mechanism adds these rules.

* An unsafe variable gets no singleton warning; advising a rename would be
  advice to disguise the bug. A rule rejected for another reason gets
  its singleton warnings, since it can be wrong in both ways at once.
* A long name is shortened and ends in `...`, so a suggested rename is
  visibly incomplete rather than naming a variable that does not exist.
* The undefined-predicate warning comes once per predicate, at the first
  atom that reads it, which is why that one location is retained.

## Failure paths and limits

| Limit | Enforced | Because |
| --- | --- | --- |
| 32 arguments in an atom | parser, at the argument | an index key is a 32-bit column mask |
| 64 variables in a rule | compiler, at the slot | the bound set is a 64-bit mask |
| expression depth | parser, twice | every walk and the evaluator's stack are bounded |
| predicate identifiers | predicate table | a wrapped identifier would merge predicates |

The arity bound is checked where the argument is written because that says
more than rejecting the predicate later, and it lets an atom's terms be
collected in a fixed buffer with no allocation or failure path.

**Expression depth is bounded twice by one constant**, one bound per
stack. The **descent of the text** -- parentheses, unary minuses and call
parentheses the parser is inside -- bounds the parser's recursion, and is
counted in the text because `((((X))))` builds one node. The **height of
the tree** -- operator, negation and call nodes -- bounds every walk and
the evaluator's stack. `cat(cat(...))` counts at both. A program the parser
accepts can then be walked, printed and run with no stack check.
**Aggregates do not nest**: one met while another is open is refused before
its body is entered, so the parser descends into at most one group body.

Nothing else is bounded: literals per body, statements per file and nodes
per expression are bounded by the text the caller supplied.

## Determinism

* Classification does not depend on the locale.
* Identifiers follow first appearance: head before body, body in textual
  order, and a load's fact values and predicates as it reads them, which
  is before the check compiles any pending rule.
* Slots follow first occurrence, head before body.
* Diagnostics come in statement order and, within a statement, in the
  textual order of what they point at.
* Generated predicate names are built from names and positions, never
  from a counter.
* No message derives from a pointer value or a hash-table walk.

## Alternatives not taken

* **A generated lexer or parser.** It adds a build dependency, and error
  recovery and byte-accurate locations are what such parsers do worst.
* **A token pushback buffer.** The grammar needs one token of lookahead,
  which the parser already holds.
* **Recovery inside a statement.** Inserting a missing parenthesis builds a
  tree the writer did not write; the terminator is the language's own
  granularity.
* **An error limit.** Messages are already bounded by statements.
* **Interning the constants and names of rules and queries as the text is
  read.** It would fill the value pool with names, spellings and the
  constants of statements that fail or are transient.
* **Declared column types, or a declared split between extensional and
  intensional predicates.** Columns are untyped, and later stages need
  only the derived mark.
* **Removal from the predicate table.** Nothing un-defines a predicate,
  and deletion would need tombstones nothing else needs.
* **A context-lifetime store for every source name.** It would grow by
  one name per session line; a location lives with what was parsed under
  its name, and only the one location the context retains is copied.
* **An entry-less read for a query atom over an unknown predicate.** It
  would stop such queries growing the table, but would hand the compiled
  rule, the dependency graph and the stratifier an atom naming no
  predicate, a case each would carry. The table's memory charge bounds the
  growth instead.
