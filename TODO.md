# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Implement relic 1.0.** [Critical] Fifteen stages S0 through S14, specified in
`relic-plan.md`. Current position: S13, differential and fuzz hardening,
landed; S14, documentation, packaging and release, next.

## Build and packaging

**Continuous integration is not configured.** [High] `relic-plan.md` section 6
specifies thirteen jobs; the repository has none, so every claim about a
platform other than macOS is untested. The tarball job matters most, because
it builds what users actually download, and the Linux jobs matter next,
because leak detection does not work under AddressSanitizer on macOS and
`make check-lsan` has therefore never run. The BSD and musl jobs need
third-party VM actions and can follow.

**Nothing enforces linking `$(LTLIBOBJS)` rather than `$(LIBOBJS)`.** [Low,
component: `Makefile.am`] A program whose `LDADD` names the plain form
brings back a second compilation of each replacement that races the libtool
one under `make -j`, and the failure is intermittent. The rule is stated only
at `LIBOBJDIR`. A convenience library carrying `$(LTLIBOBJS)` as its only
`LIBADD` would make the right spelling the only one in the file; whether an
empty convenience library builds cleanly on every platform (a platform with
no replacements) needs checking first, and a check script rejecting
`$(LIBOBJS)` is the cheaper fallback.

**`--enable-lsan` is not in the architecture's option list.** [Low, component:
`specs/architecture.md`] Section 65.4 specifies a `make check-lsan` target but
section 65.3 lists no option to build with LeakSanitizer, so S0 added one to
make the target implementable. Decide whether section 65.3 should record it.

**Two tree checks behave differently on ELF and have never run there.**
[High, component: `tests/globals.sh`] The no-mutable-global-state check has
two branches, and only the Mach-O one has been exercised, because there is no
Linux job yet. Two specific doubts to settle the first time it runs on ELF:
GNU `nm` reports `.data.rel.ro` as type `d`, so a constant table holding
pointers may be reported as mutable state and fail the build spuriously; and
the claim that a thread-local object is reported as ordinary data or bss is
inferred from how ELF places `.tdata` and `.tbss`, not observed. Settle both
by injecting each case on Linux, as was done on macOS, rather than by reading
the output once and assuming.

A third doubt joins them: the check now ignores symbols whose C identifier is
reserved to the implementation, because AddressSanitizer emits a writable
descriptor beside every global including a read-only one. The Mach-O form of
that exclusion was verified by injection; the ELF form, which matches `__` or
an underscore and a capital directly rather than after a prefix, has never
run. Verify it catches an injected `static` counter on Linux under both a
plain and an instrumented build.

**The getentropy(2) configuration is verified only on macOS.** [Low,
component: `configure.ac`, `compat/compat.h`] `AC_REPLACE_FUNCS` tests that
`getentropy` links, not that a prototype is in scope; `compat.h` relies on
`<unistd.h>` (the BSDs, glibc, musl) or `<sys/random.h>` (macOS) declaring
it, which `AC_USE_SYSTEM_EXTENSIONS` should make true. Under
`RELIC_COMPAT_DECLARE_ALL` the header redeclares the renamed interface after
the system header has declared it, as it already does for `reallocarray`;
GCC's `-Wredundant-decls` may reject that. Build `check` on glibc, musl,
FreeBSD and OpenBSD, and switch to `AC_CHECK_DECLS` if a platform links the
function without declaring it.

**The tarball carries no project documents.** [Low, component: `Makefile.am`]
`EXTRA_DIST` ships `README.md` and `LICENSE` but not `specs/`, `relic-plan.md`,
`STATUS.md` or `TODO.md`. For a tool whose selling point is that it can be
audited, shipping the architecture document with the source is worth
considering. Decide at S14 with the rest of packaging.

**`librelic` name collision.** [Low] An unrelated cryptographic toolkit
publishes `librelic` and a `relic.h`. Only matters if both install under one
prefix. Remedies are `--program-prefix` and a renamed pkg-config module.
Revisit if a distribution reports it.

## Engine

**A rejected statement keeps its arena memory.** [Low, component:
`lib/rule.c`] A statement is compiled in one pass, so a rule that turns out
to be unsafe has already had its arguments, body and variables allocated from
the program arena by the time the safety computation runs. They are not
appended to the program and cannot be given back, because an arena has no
per-allocation free. The waste is bounded by the statement, spent once for
the life of the context, and spent only on a program that is refused and will
never be evaluated. `specs/design-program.md` section 8.1 explains why the
single pass is worth it; revisit only if a caller is found that checks
enormous broken programs in a long-lived context.

**Two capacity ceilings are reported as running out of memory.** [Low,
component: `lib/relation.c`] `relic_idtab_reserve()` fails both when an
allocation fails and when the table has reached a size it cannot double, and
`relic_grow_array()` does the same; neither tells its caller which, so
`lib/relation.c` reports both as running out of memory. The ceilings need an
array of more than half the address space to reach, so the misdescription is
unreachable in practice, and distinguishing them means a second output on two
functions that have no other reason for one. It would waste a debugging
session if it ever fired. This is the same trade-off as the predicate
identifier space below, and should be settled the same way if either is.

**Exhausting the predicate identifier space cannot be retried out of.**
[Low, component: `lib/pred.c`] `relic_pred_intern()` fails either because
memory ran out or because all four billion predicate identifiers are taken,
and `lib/rule.c` cannot tell the two apart, so both leave the cursor where it
is for a later check to resume from. Retrying is right for the first and
futile for the second: a check will report the same thing every time it is
called. The condition needs about a hundred gigabytes of predicate table to
reach, so the cost of distinguishing them -- a second output on an interning
function that has no other reason for one -- is not obviously worth paying.
Revisit if a limit on predicates ever becomes configurable, since that limit
would need the same distinction.

**Diagnostics elsewhere truncate without saying so.** [Low, component:
`lib/parse.c`] `lib/diag.c`'s `relic_diag_name()` shortens a name to forty
bytes and marks the cut with `...`, which is what the program model's messages
use. `lib/parse.c` still truncates quoted token text with a bare `%.*s`
precision, so a long string literal or identifier is silently cut short in a
parse diagnostic. The two should agree. Parser text is not restricted to
identifier bytes, so a shared helper has to leave escaping to
`relic_diag_stderr()` as it does today; check that before merging them.

**Sanitizer suites cannot run from an in-place build.** [Medium, component:
`Makefile.am`] `make check-asan` and its siblings configure a fresh tree
beside the current one, and `configure` refuses to build out of a source
directory that has itself been configured, so from a tree built in place
every one of them fails before compiling anything. They work from an
out-of-tree build, which is how they were run for S5. Either the targets
should detect the in-place case and say so, or they should configure from a
pristine copy of the sources; the first is a few lines and removes a trap
from a step the working agreement makes mandatory.

**A check between loads can warn about a predicate a later load defines.**
[Low, component: `lib/pred.c`] The undefined-predicate sweep answers a
program-wide question, and an embedded caller has no end-of-program event at
which to ask it. A caller that loads one file, checks, then loads a second
file defining what the first referenced will have been warned about a
predicate that is now defined. The warning is not repeated and the program is
not refused. The command-line tool cannot hit it, because it loads every
operand before checking. Revisit if a caller is given a way to say that
loading is finished.

Prepared queries share the same sweep, which cuts both ways. A predicate a
query names and nothing defines is warned about once, pointing at the
query's text; a program loaded afterwards that references the same predicate
is then not warned about it, since the warning has been given.

**A failed load keeps its arena memory.** [Low, component: `lib/parse.c`] A
load parses onto a list of its own and splices it into the program only if the
whole input parsed, so a failed load adds no statements. What it cannot give
back is the program arena it allocated them from: an arena has no
per-allocation free, and the program arena is not reset until the context is
destroyed, because nothing replaces a loaded program yet. A long-running
embedder or a REPL that loads a broken file repeatedly therefore grows slowly.
The waste is bounded by the text the caller chose to load. The interactive
mode never loads, so it does not grow this way; it grows through the queries
it prepares instead, the item on the symbols and values of query text below.
Revisit
when a context is given a way to discard its program, which is the operation
that would reset the arena anyway.

**A stream is read whole with no size limit.** [Low, component: `lib/parse.c`]
`relic_load_file()` reads its stream into one buffer, because the syntax tree
it produces is as large as the input anyway. Nothing bounds that buffer, so a
caller that hands the library a pipe with no end waits and grows until the
allocator refuses. The `-l memory` limit of `specs/architecture.md` section 63
is about relation and index storage and does not apply here.

It stays out of that structure. The four configurable limits bound what a
program *derives* -- data the caller did not choose and cannot foresee -- and
that is why they exist. Input is the opposite: the caller picked the file or
opened the stream, knows how large it is, and can bound it before handing it
over. Folding the two into one key would make `-l memory=N` mean two things
and would still not help the one caller who cannot check, which is a program
reading an untrusted pipe. Revisit if an embedder finds that caller; the
answer there is a separate key, not a wider meaning for this one.

**An interactive session accepts only queries.** [Low, component:
`cmd/relic/repl.c`, `include/relic.h`] A session cannot add a fact or a rule,
because a load that fails its check leaves the program rejected for good, and
the tool cannot check text without loading it. Accepting statements needs a
load that is checked as one step and undone when the check fails; that is an
added entry point, a compatible change, and the session would then load a
line ending in `.` through it. Both evaluation items below -- a later load
re-evaluating everything, and complete relations evaluated per query -- would
then fall on the session.

**The symbols and values of a query's text are interned for the life of the
context.** [Low, component: `lib/parse.c`, `lib/lex.c`, `lib/symbol.c`,
`lib/value.c`, `cmd/relic/repl.c`] A constant in a query is interned as a
value, a variable's spelling and the query's source name as symbols, and no
table releases an entry. An embedder preparing a query per request over
distinct constants grows the value table by one entry per distinct constant,
and a session grows the symbol table by one entry per line, because the tool
names each line's query `<query N>` and the parser interns the name. Nothing
walks either table per answer, so the cost is memory rather than time, and
it is bounded by the distinct text seen. A query's answer leaves nothing in
the context's tables, per `specs/design-query.md` section 9; these entries
are what a query does leave. A fix is either a
source name that is not interned -- diagnostics copy it -- or a per-query
scope for the symbols and values only the query's text mentions, which is a
change to interning and wants a design note first.

**A session's line editing drops bytes outside ASCII.** [Low, component:
`cmd/relic/input.c`] The tool never calls setlocale(3), so libedit runs in
the C locale and does not accept a typed byte above 0x7f; a string literal
holding UTF-8 cannot be typed at the prompt, though it can be piped. Setting
`LC_CTYPE` from the environment before libedit starts would fix it; neither
the tool nor the library classifies bytes through `<ctype.h>`, so nothing
else would follow the locale. What needs checking first is how libedit in a
UTF-8 locale treats a typed byte that is not valid UTF-8.

**An interrupt ends the session.** [Low, component: `cmd/relic/repl.c`,
`include/relic.h`] Ctrl-C at the prompt or during a long query ends the
process, as it would a batch run, because the library has no way to abandon
a query part way and a session that caught the signal would have nothing to
stop. Once evaluation can be cancelled, a session should cancel the query
running and return to the prompt.

**A fact costs a compiled rule and a plan.** [Medium, component:
`lib/parse.c`, `lib/rule.c`, `lib/eval.c`] A fact is a rule with an empty
body, so each one is a syntax tree node, a compiled rule and, at every
evaluation, a one-operation plan in the evaluation arena. A million `edge/2`
facts, 21 MB of text, take 1.8 s and 260 MB to load and check and about
450 MB resident to evaluate, while the relation holding them is 24 MB;
`tests/perf.sh` measures this shape. Loading facts of a component straight
into a staging relation, or planning a component's facts as one bulk
operation, would remove most of it without a second path for rules.
`relic_fact_add()` pays the same per fact, since it appends the statement its
text would load.

**A later load re-evaluates the whole program.** [Low, component:
`lib/eval.c`] Any change to the program after an evaluation discards every
relation and computes the fixed point again from nothing, for every program:
continuing from the tuples already held is sound without negation and unsound
with it, and one rule for both is what `specs/design-eval.md` section 6
settles, matching architecture section 54 for `relic_fact_add()`. The cost
falls on a caller that loads a large positive program and then adds to it a
little at a time -- an embedder -- which pays for the whole
program on every addition. Measure it there before doing anything; the
remedy, if one is needed, is incremental maintenance, which architecture
section 70 defers, and not a return to continuing only positive programs.

**Complete relations are evaluated again for every query.** [Medium,
component: `lib/eval.c`] A query answered through its Magic rewrite discards
every relation first and evaluates again the components it reads completely,
so a caller running many bound queries over one program loads the facts and
evaluates the negated and extensional part once per query. What that part
holds depends only on the program, so it could be kept until the program
changes, with only the generated relations and the answer discarded between
queries. `specs/design-magic.md` section 9.2 chose the simpler rule so that
what a context holds after a query depends on that query alone; measure a
REPL or embedder workload before changing it.

**A query can derive more through the rewrite than without it.** [Medium,
component: `lib/magic.c`] Each adornment of a predicate derives a relation of
its own, and the budget bounds the rules generated, not the tuples those
relations hold. A query that binds nothing reads its predicate through the
all-free adornment, but the rules it reaches may still call that predicate
with a column bound: `path(X, Y)?` over
`path(X, Z) :- edge(X, Y), path(Y, Z).` derives `path$ff` and `path$bf`,
each about as large as `path`, so it holds about twice the tuples of the
whole fixed point. A program whose rules call one predicate with several
bound patterns does worse: one with a ternary predicate called five ways held
five times the fixed point's tuples. The remedy is subsumption: once `p$ff`
is demanded, every other adornment of `p` can read it filtered instead of
being derived again, and more generally an adornment can read one that binds
a subset of its columns. A cap on adornments per predicate, falling back to
the all-free one, would bound the multiple. The rewrite is correct as it
stands, the tuples count against `max_tuples` and `max_memory`, and
`tests/perf.sh`, whose query binds nothing, stays inside its bound.

**A mixed predicate's facts are copied per adornment.** [Medium, component:
`lib/magic.c`] A predicate derived by facts and rules alike is read on demand,
so each of its facts becomes a rule guarded by the magic predicate, once per
adornment. A predicate with many facts and a single rule pays for all of them
in rules and plans: 200 000 `e/2` facts and one rule for `e` take 296 MB and
1.0 s for `e(n7, X)?`, against 129 MB and 0.45 s without the rewrite, and
375 MB when the query calls `e` with two adornments. The copies and their
plans are charged to `max_memory` (D-18), so under a limit such a query is
refused while `-X no-magic` answers it in a fraction of the memory: 20 000
facts and one rule, `e(n7, X)?`, need 16 MB through the rewrite and under
2 MB without it. A limit refusing the rewrite fails the query, as
`specs/design-planning-cost.md` section 7 decides, rather than falling back
to the whole program as an outgrown budget does; revisit that if this case
matters. Without a limit, the budgets bound it at about sixteen copies of
the program. Splitting such a predicate into its facts, loaded completely
into a relation of their own, and its rules, adorned, with one guarded rule per
adornment reading the facts, would avoid the copies and would make the
extensional case of `specs/design-magic.md` section 3 a special case of the
general rule rather than a rule of its own.

**An abandoned rewrite repeats the complete part.** [Low, component:
`lib/eval.c`, `lib/explain.c`] A rewrite over its budget is abandoned after
the components it reads completely have been evaluated, and answering from
the whole program then discards them and evaluates them again. `-e` shows
both passes, so `tests/lang/magic-budget.dl` under `-e` prints
`stratum 0, component 0: f/14` twice with nothing between them to say why;
the warning goes to standard error. An explain line marking the abandoned
rewrite would say why, and keeping the complete relations, as the item on
evaluating them for every query proposes, would avoid the second pass.

**Generated names are interned for the life of the context.** [Low,
component: `lib/magic.c`, `lib/rule.c`, `lib/symbol.c`] Every adorned and
magic predicate a rewrite generates, and the symbol naming it, stays in the
context's tables after the query, as every interned entry does. The same
query run again reuses them, so the growth is bounded by the distinct adornments ever asked
for, at most the budget per distinct query shape; a long-lived embedder
answering many differently shaped queries grows by all of them. Releasing a
rewrite's generated entries would need the tables to support removal, or
generated predicates to live in a table of the rewrite's own.

**A limit reached while deriving a generated predicate names it.** [Low,
component: `lib/magic.c`, `lib/relation.c`] `relic -l tuples=10 -q
'path(a, X)?'` reports `limit exceeded: tuples=10 while deriving path$bf/2`,
where `-X no-magic` says `path/2`. The name is unambiguous -- `$` cannot occur
in a name a program writes -- and `-e` shows what it means, but
`specs/design-magic.md` section 8 asks a diagnostic to name what the reader
has to change. Recording the source predicate on a generated predicate's
entry and describing it from that would say `path/2`.

**The rule budget counts a body's demand calls twice.** [Medium, component:
`lib/magic.c`] A body of k demand calls generates a magic rule and, after
the first, a supplementary rule per call, about 2k rules, and both count
against `max_magic_rules` and against the rule budget of 256 + 16 x the
program's rules. With `-l magic-rules=1000`, `r(X) :- p(X), ..., p(X)` of
600 calls over five facts is refused, where writing the prefixes out in
full kept 602 rules and answered it, at 15 s and 5.9 GB; without a limit,
the same body of 200 calls outgrows its rule budget of 368, where the
literal budget, 256 + 16 x its size in literals, would admit it.
`specs/design-magic.md` section 8 keeps supplementary predicates out of the
predicate budget so as not to abandon a body of hundreds of calls; a rule
budget sized by rules does just that, and the literal budget already bounds
supplementary rules. Size the rule budget by the program's literals, stop
counting supplementary rules, or retire the budget as the next item asks.

**The literal budget may make the rule budget redundant.** [Low, component:
`lib/magic.c`] A drafted rule is about as long as the rule it comes from,
and the literal budget counts every draft against 256 + 16 x the program's
size in literals, so it fires before the budget on generated rules on every
program measured, `tests/lang/magic-budget.dl` included; the rule budget
fires first only on a program whose unreached rules are long, which raise
the literal budget and generate nothing, and no test reaches it. The
predicate budget is not redundant: facts of extensional predicates raise
the literal budget and not the predicate budget, and `widening_program(12)`
from `tests/unit/programs.c` with 50 000 facts of an unrelated g outgrows
its budget of 304 generated predicates first; no test reaches that either.
Weigh whether the rule budget is worth keeping. The configurable limits are
not in question.

**Append-only invariant versus incremental maintenance.** [Medium, component:
`lib/relation.c`] D-10 represents a delta as a range of tuple identifiers,
which requires that nothing compacts, deletes or renumbers a relation during a
fixed point. The store holds to it by construction: nothing removes, reorders
or renumbers one tuple, and the one removal there is, discarding every
relation at once, runs only before an evaluation begins, when no range is
live. The debt is the moment that stops being enough. Any incremental
insertion or DRed-style deletion support has to confront it first: a range
captured before such an operation means something else afterwards, and
nothing in the range would say so. `relic_fact_add()` needs no more than the
existing discard, because it appends a fact statement to the program rather
than inserting a tuple into a relation: only the evaluator inserts,
which is what makes every tuple re-derivable and the discard safe, and
appending a rule is what advances the count that says the program changed.
`specs/design-relations.md` section 2 states it.

**Size budget.** [Medium] `relic-plan.md` totals about 7.5 KLOC against the 4
to 7 KLOC of `specs/architecture.md` section 2.2, which treats size as a design
constraint. Track actual lines per stage against the budget; the levers are
listed under risk R7.

S2 came in at about 1200 non-comment lines against a budget of 880, roughly
a third over. Most of the excess is the three loading entry points, the
whole-stream reader and the `-c` handling, which the stage's delivery list did
not enumerate because it assumed `relic -c` came free. Decide at S5, when the
tool's real size is known, whether to record the front end's budget as having
been understated or to find the lines back.

S3 came in at about 700 non-comment lines against a budget of 600, a sixth
over, which is inside the plan's stated tolerance. Risk R7's first lever is to
merge `lib/pred.c` into `lib/rule.c`, which architecture section 64 already
permits; that would save the file header and a handful of accessors and is
worth doing only if a later stage needs the lines.

S4 came in at about 400 non-comment lines against a budget of 600, a third
under, which gives back most of what S3 overran. The store is small because
the identifier table and the growable array were already there and the memory
limit needed two pure projections from them rather than a mechanism of its
own.

S5 came in at about 1910 non-comment lines against a budget of 1670, a seventh
over. The library is about 1060 against the 1070 its share implies, and the
tool about 850 against 600, most of it `cmd/relic/output.c`: four formats
that each escape differently, a UTF-8 validator for json and a merge sort for
`-s`. The S2 question is therefore answered: the tool's budget was understated
along with the front end's, by the formats rather than the option parsing.
Risk R7's second lever, dropping one of csv and json, is the place to find the
lines back if a later stage needs them; neither has proved redundant yet.

S6 came in at about 870 non-comment lines against a budget of 500, three
quarters over. The graph and its iterative search are about 280, close to what
the budget imagined for the whole stage. The excess is in `lib/stratify.c`:
about 180 lines for the unstratified diagnostic, which picks the first
offending edge of each component in text order and finds the shortest cycle
through it by a breadth-first search, and about 150 for the schedule, which is
built completely before it replaces the old one so that running out of memory
leaves a consistent program. The diagnostic was specified by the plan and the
atomic replacement by the working agreement's exhaustion rules, so neither is
a lever; the budget understated them.

S7 came in at about 360 non-comment lines against a budget of 350. Most of
it is `lib/eval.c`, where a component's plans became a structure of their own
-- base plans, delta variants and the members' relations -- and the rest is
`-X` and the counters in the tool and the header.

S8 came in at about 85 net non-comment lines in the library against a budget
of 150, as the plan predicted: negation added one operation and a placement
rule the comparisons already had. `-l`, brought forward from S12, added about
60 to the tool, which S12's budget of 450 was to give back.

S10 came in at about 1550 non-comment lines against a budget of 800, nearly
double, counting the budget on the rewrite and the linear walks it needed
once review found a 2^14-adornment program and a quadratic cycle.
`lib/magic.c` is about 1100 of them: finding what a query reads
completely, the adornment memo, renumbering generated rules into a canonical
form, and structural duplicate detection with a hash of its own. The rest is
splitting the planner's ordering from its emission, giving the analysis and
the evaluator a rule table to work over, and nested explain. Generated rules
are ordinary compiled rules, which is what kept the evaluator unchanged; a
lighter representation for them is not a lever while that holds.

S11 came in at about 390 non-comment lines against a budget of 250, about
430 in the library less about 40 the tool gave back by collecting through
it. The interface itself is small; the lines are the entry and exit at
every mutating entry point with the workers that let entry points share
code, argument validation for the entry points taking caller data, and the
collector.

S12 came in at about 610 non-comment lines against a budget of 450, and
did not give back the 60 of `-l`: about 520 in the tool, 240 of them the
session and its commands in `cmd/relic/repl.c` and 190 the line reader in
`cmd/relic/input.c`, most of which is the two ways of reading a line and
libedit's setup; and about 90 in the library for the mixed-type note. The
tool now stands near 1,650 against the architecture's 400 to 600, the
overrun of risk R7 concentrated in its output formats and its interactive
mode.

**A later load's second unstratified cycle is not reported.** [Low,
component: `lib/stratify.c`] A program found unstratified is refused by every
later check without being analysed again, so an embedder that checks between
loads is told about the first cycle and not about an independent one a later
load introduces. The command-line tool loads every operand before checking and
cannot hit it. Revisit if an embedder needs every cycle; re-analysing
a rejected program and reporting only components not already reported would
do it.

**An unknown value type cannot be rejected in a build that checks for it.**
[Low, component: `lib/value.c`] `value_well_formed()` rejects a
`relic_value_t` whose `type` is not one of the three enumerators, which an
embedding program can construct. The guard is unreachable under
`-fsanitize=undefined`: reading an out-of-range enum is undefined, so
UndefinedBehaviorSanitizer traps on the load before the guard runs, and in
that build the library aborts its host rather than returning `-1`. The guard
is kept because it costs three lines and catches the mistake on an ordinary
release build, but it cannot be a guarantee and `tests/unit/test_value.c`
leaves the case untested for the same reason. Making it real would mean
taking the type as an integer across the public boundary, which changes the
representation `specs/architecture.md` section 19.1 specifies. Revisit if an
embedder hits it.

**Assertions are absent from release builds.** [Medium, component:
`configure.ac`] `specs/architecture.md` section 65.3 makes assertions a
`--enable-debug` feature, so an ordinary build defines `NDEBUG`. Sanitizer
builds keep them, which is where they earn their place. The consequence is a
rule for later stages rather than a defect: an invariant that must hold in a
release build needs an explicit check and a diagnostic, not an `assert()`.
The append-only invariant of D-10 is the first one this applies to.

**Coverage gaps.** [Medium, component: `tests/`] `make coverage` reaches
96 percent of the library's and the tool's lines. What it does not reach is
sorted in `docs/todo-coverage.md`: arena allocation failures the exhaustion
sweep cannot land on, which one arena-level fault point would reach; the
tool's allocation failures, which nothing injects; two `fclose(3)` failures
no portable test can cause; defensive branches, three of which have lost
their reason to exist; and the terminal path.

**Allocation-failure coverage must grow with each stage.** [Medium,
component: `tests/unit/test_exhaustion.c`] The suite compiles the library
against an allocator that refuses the nth request and sweeps n across symbol
and value interning, across a load, across a check, across the dependency
analysis and the replacement of a schedule, across a batch of tuple
insertions, and across preparing and answering a query over a recursive
program, which is what caught the index-growth failure that broke interning
identity, and across building a secondary index and inserting into an
indexed relation, and across a query answered through its Magic rewrite.
Every later stage adds allocation paths that the sweep will not reach unless
it is extended. Add a scenario per stage rather than leaving the suite
describing the stages that happened to write one.

Each new scenario needs the straddle guards the existing ones have. The
insertion sweep was written first over a single insertion into a relation that
already had room, so it refused nothing and reported success without testing
anything; the guard that the sweep must reach both a refusal and a completion
is what said so.

**Leak detection is unavailable on macOS.** [Medium, component: `Makefile.am`]
AddressSanitizer on Apple silicon reports that `detect_leaks` is unsupported
and aborts, so `configure` probes for it and `make check-asan` asks for leak
detection only where it works. `make check-lsan` is unavailable there for the
same reason. Until the Linux continuous integration jobs exist, no leak check
runs anywhere, which matters from S1 onward because that is where the tree
starts allocating.

**`librelic.pc` declares no private dependencies.** [Low, component:
`librelic.pc.in`] There are none today. The moment the library links anything
of its own, libedit included, `Libs.private` and `Requires.private` have to
name it or `pkg-config --static --libs librelic` will produce a link line
that does not work. `tests/pkgconfig.sh` checks that the flags are
expressible, not that they are complete.

**The sanitizer suites do not inherit the surrounding configuration.** [Low,
component: `Makefile.am`] `make check-asan` and its siblings configure a fresh
tree carrying over only `CC`, so a developer's `CFLAGS`, `--enable-werror` or
`--prefix` are absent from the instrumented run. That is deliberate, so that a
sanitizer run means one thing regardless of how the working tree was
configured, but it means the instrumented build is not the build being worked
on. Revisit if the difference ever hides something.

**The program-name check does not run in a libtool-wrapped build.** [Low,
component: `tests/version.sh`] `relic` rejects an `argv[0]` whose last
component is not printable ASCII, so a filename carrying a terminal escape
sequence cannot reach a diagnostic. Where the tool links an uninstalled
shared library, libtool wraps it in a script that re-executes the real binary
under a name of its own, which puts `argv[0]` out of reach; the test detects
that and skips. It runs under `--disable-shared` and against an installed
binary, so the property is covered, but not in the default build.

**The interactive mode's terminal path is not tested automatically.** [Low,
component: `tests/`, `cmd/relic/input.c`] The prompt, libedit's editing and
history, and the end of input at a prompt happen only when standard input is
a terminal. They were checked by hand through a pseudo-terminal in builds
with and without libedit, but driving one needs more than `sh`: `script(1)`
differs between the BSDs and Linux, and on macOS it forwards the end of its
own input before the session has started. A small pty driver built as a
check program would cover it.

**The truly unterminated `strlcat` destination cannot be tested.** [Low,
component: `compat/strlcat.c`] The guard against a destination holding no
terminator is exercised with a buffer whose terminator lies beyond the size
passed to the call, and with a stated size of zero. A buffer with no
terminator anywhere is not a string, and AddressSanitizer's
`strict_string_checks` rejects the fixture before the function runs. The
arithmetic the guard protects is covered; the exact input a caller would pass
by mistake is not.

**A query's width is known only once it is collected.** [Low, component:
`include/relic.h`, `lib/rule.c`] `relic_result_columns()` gives a query's
number of output columns, but only after collecting it; a caller streaming
rows who wants to write a header first has neither the width nor the
variable names the columns stand for. `relic_query_columns()` and
`relic_query_column_name()` would supply both. Adding functions is a
compatible change, so this can wait for a caller that needs it.

**The manual's prototypes are not checked against the header.** [Low,
component: `tests/man.sh`] The test holds the `NAME` list of `man/relic.3` to
the header's exported functions, but not the `SYNOPSIS` prototypes or the
structures the page reproduces, so a changed parameter would leave the page
wrong without failing anything. Comparing each `.Fn`/`.Fo` block's types
with the header's declaration would close it.

**Adding a counter or a limit is a new major version.** [Low, component:
`include/relic.h`] `relic_config_t` and `relic_stats_t` are complete
structures the caller allocates, so appending a field changes the size a
caller compiled against, and the library's version-info must then start a
new major version: a step limit, or any new counter, forces every
embedder to rebuild. Passing the size the caller compiled against was
considered and not taken; revisit if the first such change is expensive.

**A memory-limit refusal can name a generated predicate.** [Low, component:
`lib/eval.c`] A component of a query's rewritten program is named by its
first member, so a limit refusing its plans reads `limit exceeded:
memory=N while planning magic$p$b/1` or `... e$bf/2`, names the user never
wrote. Naming the query instead, `while planning $q0/1`, for every component
of a rewritten program would say what the user asked for; it needs the
evaluation of a schedule to be told whose it is.

**The limit message is formatted in three places.** [Low, component:
`lib/relation.c`, `lib/magic.c`, `lib/diag.c`] `limit exceeded: %s=%zu while
%s` is written by the relation store, the Magic rewrite and
`relic_diag_no_memory()`. One `relic_diag_limit()` would make "the form every
limit shares" hold by construction. The eval fuzz target already depends on
the form, telling a limit's refusal from a failure by the prefix `limit
exceeded:`, so a limit worded otherwise would read as a finding there; an
error class a caller can ask for would free both from the text.

## Benchmarks

**No run is recorded on an idle machine.** [Medium, component: `bench/`]
`specs/design-bench.md` section 8 keeps one comparison run in
`bench/compare/results.txt`, which does not exist yet: it needs an idle
machine, ten timed runs per cell and the scale sweep, `BENCH_SCALE` 1, 2, 4
and 8, so the curves show which gaps grow with the input.
`bench/baseline.txt` was recorded on a machine busy with other work; its
counters are exact, but its seconds are longer than an idle run's and want
recording again in the same session.

**CSPA has not run on the Graspan data sets.** [Medium, component:
`bench/compare/`] `specs/design-bench.md` sections 4.3 and 9 run CSPA over
the Graspan inputs from a download cache with checksums; only the generated
program of section 4.3 is run, and the cache, its checksums and its
`.gitignore` entry come with the first external data set. The inputs have
no stable public URL -- the papers that use them got them from the RecStep
authors -- and the smallest, httpd, derives about 2.3e8 `valueAlias` and
8.9e7 `memoryAlias` tuples from 1.5 million facts, 3.2e8 with the 1.4e6
of `valueFlow`: 15 to 30 GB at the 47 to 92 bytes of peak memory per held
tuple relic reached on generated Andersen programs.

**The same-generation program is rbench's, unchecked against
OpenRuleBench's own.** [Low, component: `bench/compare/sg-*`] Set B's `sg`
starts from `sg(Y, Y) :- par(_, Y)`, as rbench's rendering of OpenRuleBench
does. OpenRuleBench describes the problem as same-generation siblings, and
its original programs, no longer at their published address, may start from
siblings, `par(X, Z), par(Y, Z), X != Y`, instead. Find the original; if it
differs, follow it, since section 4.1 admits the inequality.

**A bound right-recursive closure costs relic more than the whole
closure.** [Medium, component: `lib/magic.c`, `bench/compare/tc-cyclic-bf`]
On the dense cyclic graph, `tc(500, X)?` over `tc(X, Z) :- par(X, Y),
tc(Y, Z)` took relic 28 s against 17 s for `tc(X, Y)?`, measured on a busy
machine. The Magic rewrite carries the binding through the recursive call to
every node reached, which here is every node, so it computes the whole
closure and pays for the magic relation besides. The rewrite is correct but
the query's answer is only the nodes reachable from 500. `bench/run.sh`
shows the same on a chain: the right-recursive bound query derives two
million tuples for an answer of 2000, the left-recursive one six thousand.
A right-linear recursion bound on its first argument can be answered by
reachability over `par` alone (the right-linear and factoring rewrites of
Naughton, Ramakrishnan, Sagiv and Ullman); find whether relic's rewrite can
recognize the case. Souffle's `--magic-transform` produces the identical
program and runs it in about 0.9 of relic's time, so the gap to Souffle is
in evaluation rather than in the rewrite: see the next item.

**relic's evaluation costs several times Souffle's interpreter per derivation
on the analyses.** [Medium, component: `lib/eval.c`, `bench/compare/andersen`]
Measured on a busy machine, `andersen` took relic 1.9 s against 0.7 s for the
Souffle interpreter. The gap widens on denser inputs. `dataset_andersen` at
2000 functions, with each function's three arguments assigned to variables of
functions drawn uniformly from the whole program instead of received from `(F
- 1) / 2`, makes about 1.1 million points-to facts from about 380 million
derivations and 960 million lookups, and took 248 s of user time in relic
against 17 s; the exact figures depend on the draws. `tc-nonlinear`'s program
over the `sparse-cyclic` data set ran past 600 s in relic against 117 s in the
interpreter. Semi-naive evaluation does not repeat work here -- on a chain the
nonlinear closure makes about 2.5 derivations per path triple at every length
-- so the gap is the cost of a derivation and its lookups, not their number.
Profiles of `tc-cyclic` and `sg-cyclic` put about three quarters of the time
in the duplicate check of `relic_relation_insert()`, split between
`relic_idprobe_next()` and the tuple comparison. The relation's hash table
holds only tuple identifiers, so every candidate slot costs a read of the
tuple it names, at a load factor up to three quarters, and a table of a
million tuples does not fit in cache. Souffle keeps the same relation in a
B-tree and inserts with the outer loop fixed on the head's first column, so
consecutive inserts land in the same leaf. Two remedies, to be measured
separately: store a hash tag beside each identifier (or, for arity two or
less, the tuple itself) so that a slot rejects a mismatch without a tuple
read; and, once the views of the previous item are bounded, buffer an
iteration's derivations and insert them in batches, prefetching or
partitioning by hash, instead of one random probe per derivation. Designed in
`specs/design-evaluation-cost.md` sections 5 and 6.

**Loading facts costs relic more memory than any other engine.** [Medium,
component: `lib/`, `bench/compare/facts-join`] On `facts-join`, 300,000
ternary facts read as source text, relic peaks at about 160 MB, against
44 MB for Souffle reading the same data as TSV and 90 to 115 MB for Nemo,
clingo and SWI-Prolog, which also parse. Find where the peak comes from --
the syntax tree of a fact file held whole, the value table, the relation --
before deciding whether a streaming fact loader or the CSV import of
`specs/architecture.md` section 69 is the remedy. Time follows the same
path: loading is about half of `facts-join` at scale 4 and 0.07 of the 0.08 s
of user time `graph-left` takes, which is most of Souffle's lead on
`graph-left`, `graph-reach`, `facts-join` and `tasks-ready`. A symbol's
bytes are hashed once, by the lexer into the symbol table; the value table
hashes the symbol identifier, one word. The symbol table recomputes every
symbol's byte hash when it grows (`symbol_rehash()`), which
`specs/design-evaluation-cost.md` section 5 removes; section 7 of that
design records that the rest of the cost is a fact's representation as a
syntax tree statement and a compiled rule beside its tuple, and defers it
to a design of its own.

**Souffle and Nemo write their answers to files, the others to a discarded
standard output.** [Low, component: `bench/compare/run.sh`] Under load, a
Souffle run writing its answer file showed wall time far above its CPU time,
and one writing to standard output did not, but the difference was within
the noise of a busy machine. Settle on an idle one whether the file costs a
measurable amount; if it does, time Souffle with `-D -` and find Nemo an
equivalent.

**The tasks data set has almost no done tasks.** [Low, component:
`bench/datasets.sh`] `dataset_tasks` makes a task done two times in three,
and only if every task it depends on is done, so the done set dies out
early: at scale 1, 54 of 10000 tasks are done, and `tasks-ready` asks for
a frontier far smaller than the project the generator's comment describes.
Choose a rule that keeps a real share of the project done -- a higher
chance, or dependencies drawn mostly from recent tasks -- and record the
counters again.

**An interrupted comparison leaves the engine it was timing running.**
[Low, component: `bench/compare/run.sh`] On SIGTERM or SIGINT the harness
exits and removes its work directory, but `hyperfine` and the engine under
it, or the `timeout` of a verification run, are not signalled; an engine
past its data keeps running, orphaned, until it finishes on its own. Stop
the children the harness started without signalling a process group it
does not own, which `kill 0` would when the harness runs without job
control.

**Published numbers need a second platform.** [Low, component:
`bench/compare/`] Section 8's criteria include a machine of a different
operating system and processor; every run so far is one macOS Xeon.

## Security residuals

**A long-lived context gives a timing oracle on its hash seed.** [Low,
component: `lib/util.c`] The seed defeats crafted collisions because an
author cannot learn it, and a batch run gives nothing to learn from. A REPL
session (`-i`), or an embedding that feeds one context facts from an
untrusted party over time, lets that party time its own insertions against
one fixed seed and, in principle, search for keys that share a cluster.
No attack has been built. Decide whether a context used this way should be
documented as trusting its input, or whether a stronger keyed hash for the
byte strings the symbol table interns is worth its cost there.

**A dropped rule's body stays in the rewrite arena.** [Medium, component:
`lib/magic.c`] `publish_rule()` canonicalizes a draft, which copies its
body into the arena, before `rule_derives_nothing()` and the duplicate
check, so a rule that is then dropped still costs its body in memory.
Supplementary predicates keep every magic rule's prefix after a body's
first demand call to one literal, wherever its live variables fit a
predicate, so what a dropped rule can cost is the prefix before the first
demand call, or an adorned rule copied whole. A cut can also hide a
tautology: in `p(X) :- q(X), p(X).` adorned `b`, with q derived by a rule
and so read on demand, the call on p is the second demand call, and its
magic rule reads the cut rather than the guard,
`magic$p$b(X) :- sup$p$b$1$2(X).`, and is kept, with a supplementary
relation that holds nothing the guard does not and a recursive component
of the two; the answers are right and the work is wasted. With
no Magic limit configured, the literal budget charges every draft, dropped
or not, so this is bounded with the rest of the rewrite and is a matter of
memory efficiency: the arena is charged to `max_memory` (D-18), and a
dropped rule's body is memory the user pays for and gets nothing from. A
configured limit lifts the budget and counts only the rules kept, so there
nothing bounds the drafts but `max_memory`: a rule of two thousand
constant literals deriving p/11 from itself, behind the widening rules of
`tests/unit/programs.c`, takes 1.9 GB under `-l magic-rules=100000`.
Deciding derives-nothing on the draft before `canonicalize()` closes the
memory by construction; not recording, and not cutting before, a call whose
adornment and bound arguments are the head's own closes the hidden
tautology as well.

**No limit bounds the work of a join.** [Low, component: `lib/eval.c`,
`include/relic.h`] The limits count tuples held and bytes allocated, and a
join over small relations whose every result is a duplicate -- a cross
product of a few thousand tuples projected onto one column -- stays inside
both while its time grows with the product. `specs/design-api.md` leaves a
step or time limit out deliberately, so an embedder evaluating untrusted
programs has to bound the time itself, and the eval fuzz target leans on
libFuzzer's `-timeout` for the same reason. Adding one is the
`relic_config_t` change the item above describes; decide before 1.0
whether it belongs in the frozen interface.

**csv and tsv carry raw control bytes.** [Low, component:
`cmd/relic/output.c`] The text format writes a string as a quoted literal with
its control bytes escaped, so a program cannot put a terminal escape sequence
on the reader's screen through its results. csv and tsv are byte-transparent
machine formats, per `specs/design-cli.md`, and write a string's bytes as they
are, so `relic -F csv` over an untrusted program, printed to a terminal rather
than piped to a parser, can. That is the same exposure as `cat` on the
program itself, and escaping would corrupt the data for the consumers the
formats exist for; recorded so that the choice stays deliberate.

## Post-1.0

Candidate features, in the priority order of `specs/architecture.md` section
69. None may change the language semantics of v1.

* CSV and TSV fact import
* arithmetic expressions
* incremental insertion maintenance
* DRed-style deletion maintenance
* aggregates
* parallel SCC evaluation
* persistent compiled rule plans
* relation export
* better cardinality estimation
* further Magic Sets and negation optimization
