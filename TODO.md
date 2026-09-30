# relic open work

Forward-looking index. Closed items are deleted; git history is the record of
what was done. Each item carries a severity and a component pointer.

## Major initiatives

**Implement relic 1.0.** [Critical] Fifteen stages S0 through S14, specified in
`relic-plan.md`. Current position: S13, differential and fuzz hardening,
landed; S14, documentation, packaging and release, next.

**Close the comparison gaps.** [Medium] Stage S15 of `relic-plan.md`,
specified in `specs/design-comparison-cost.md`. The six changes of the
design's section 10 are in, each measured against the one before it in
`STATUS.md`. At scale 1 relic is ahead of every interpreter and SQL
engine of `bench/compare/run.sh` on all 25 workloads, and behind compiled
Souffle on `tc-cyclic`, `tc-cyclic-bf` and `grid-reach`. Three of the
section 10 gates are missed, and the stage is not done until they hold and
the comparison is recorded on an idle machine. Step 1's writer is 11 per
cent of `chain-closure` against the 5 its gate asks for; what remains is
formatting 24 MB of rows rather than the stream's lock, and no cheaper
formatting is designed yet. Step 5's peak and step 6's import have items of
their own: a growth's slack and transient keep the peak above the live
heap, and importing facts misses the comparison's gate.

**Parallel evaluation misses its speed-up gates.** [Medium, component:
`lib/round.c`] Steps 1 to 4 of `specs/design-parallel.md` section 15 are in
and every identity gate holds, but `tc-cyclic` is 5.3 times faster at
sixteen threads than at one, against the six step 3 asks for and the ten
step 4 asks for, and steps 5 and 6 are not started. The gaps and what to
try are sorted in `docs/todo-parallel.md`: the recursive rule's first round,
about half of a sixteen-thread run, stays on the calling thread because an
item that has not run is estimated at its view rather than its join; the
sharing threshold was set before the merge was shared; the largest round
scales about eight times on sixteen threads; a round that inserts most of
what it derives scales less, so `dist/3`, 2.7 times, misses the speed-up
gate of step 4 of `specs/design-arithmetic.md` too; and the page records a
comparison with Souffle built with OpenMP, which leads on `tc-cyclic` and
trails on `cspa` and `chain-closure`.

**Interning outside evaluation grows the integer table unchecked.**
[Medium, component: `lib/value.c`] `specs/design-arithmetic.md` section 9.1
charges the integer table to the memory limit whole and checks it before
each growth; only an evaluation's batch asks before growing it.  An integer
a load, an import, `relic_fact_add()` or a prepared query interns is
counted and not refused, so preparing and destroying queries with distinct
integer constants grows the table past the limit, as the symbol and string
tables already grow outside it.  A load's facts are refused by their
store's growth, which counts the table.  The fix is one reservation path
that asks the limit before either kind of interning grows the table, with
a diagnostic for an interning that names no relation, and the limit sweeps
of `test_index`, which intern before they insert, taught that a value may
be refused.

**The v1 specs do not state arithmetic.** [Low, component: `specs/`]
`specs/design-arithmetic.md` section 19 lists amendments to the other
design documents that were never made, so the aggregate text those
documents now carry sits beside v1 wording that omits assignments:
`specs/design-parser.md` still says there are no expressions and has no
operator token, and its section 5 counts five types;
`specs/design-program.md` section 5 has no closure, its section 6 messages
lack "or an assignment", and its section 4.1 says every slot is bound by a
positive relational literal; `specs/design-plan.md` has no `ASSIGN`, and
its section 9 says a comparison that binds is absent;
`specs/design-eval.md` section 8 omits the undefined arithmetic result;
`specs/design-magic.md` section 6.1 does not renumber expression trees;
`specs/design-parallel.md` section 12 has no `integer_values` identity;
`specs/design-cli.md` section 9 omits the arithmetic note; and
`specs/architecture.md` section 13's grammar has no expression.

**An arena refusal is reported by every caller that meets it.** [Low,
component: `lib/eval.c`, `lib/diag.c`] `relic_diag_no_memory()` reports each
call, and an arena that refused once refuses every later request, so a
caller that makes two allocations before checking either reports one
refusal twice, the second time as "out of memory" after the limit's
message.  Every caller must check each allocation before the next, a
convention held by enumeration; a query's group run once broke it.  An
allocation helper that reports only the refusal that first failed its
arena would make the convention unnecessary.

## Build and packaging

**Continuous integration is not configured.** [High] `relic-plan.md` section 6
specifies fifteen jobs; the repository has none, so every claim about a
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

**A round's merge scans its arrays once per stripe.** [Low, component:
`lib/merge.c`, `lib/relation.c`, `lib/index.c`] Each part of finding the
new tuples reads every candidate's stripe byte to take its own, and each
part of classifying, placing and linking reads every staged tuple's, so
the reading grows with `RELIC_STRIPES` rather than with the threads. At
sixteen stripes it is a byte per candidate per part; were the stripe
count raised far beyond that, a counting sort by stripe, once per step,
would replace the scans.

**The allocation-failure suite's deferred sweeps take a minute.** [Low,
component: `tests/unit/test_exhaustion.c`, `lib/merge.c`] A deferred run
of the sweep's program makes some 3600 to 4800 allocations, against 250
to 400 undeferred, most of them the merge's, which allocates its staging,
a table of firsts per stripe and a table of new keys per key stripe afresh
every round; the sweep runs the program once per allocation, so
`test_exhaustion` takes about 63 s in a debug build. Carving a round's
scratch tables from one charged block would cut both, at the price of an
allocator of the merge's own.

**Striped tables multiply the number of mappings.** [Low, component:
`lib/util.c`] Every stripe's arrays are mapped once they reach
`RELIC_ARRAY_MAP_BYTES`, so a large program holds several times the
mappings it did with unstriped tables. On Linux the default
`vm.max_map_count` of 65530 would be reached with some 8 GB of arrays just
past the 128 KiB threshold, rather than 64 GB at a mebibyte. Unverified;
worth measuring on Linux before a release that promises tens of gigabytes.

**A mapped array's page rounding grows with the page size.** [Low,
component: `lib/util.c`] A mapped array is a power of two of items behind a
header, so its mapping always rounds up by one page more. Beside the
smallest mapped arrays, of 128 KiB, that is 3 per cent with 4 KiB pages, 12
per cent with the 16 KiB pages of Apple silicon and half again with 64 KiB
pages, and the rounding is not counted in the storage total. The threshold
was measured on a 4 KiB-page machine only; measure on arm64, or tie it to
the page size, such as the larger of 128 KiB and 32 pages.

**A small relation pays for every stripe its keys land in.** [Low,
component: `lib/index.c`, `lib/relation.c`] The canonical index and every
secondary index are sixteen identifier tables, each allocated at its
smallest size once it holds a key, so a relation of a few dozen tuples
holds up to sixteen tables where one would do: the golden programs' storage
grows from 110 KB with one stripe to 175 KB with sixteen, and
`tests/lang/stats-indexed` from about 1.4 KB to 5.6 KB. Large relations pay
nothing measurable. Keeping a table unstriped until it holds a stripe's
worth of keys, and splitting it once, would remove the cost, provided the
split happens at the same count at every thread count so storage bytes stay
identical.

**The discard's mark is set where a relation is handed out, not derived.**
[Low, component: `lib/relation.c`] `holds_derived` is set by the write
funnel and by an index build, and the discard returns at once when it is
clear. That is right because nothing writes a derived relation through a
pointer handed out before a discard, which holds today since every
evaluation discards before it plans and nothing commits while one runs; a
future path that kept such a pointer would leave derived tuples the next
discard skips. Derive the property instead: count the base stores' tuples
in the store, bumped only by `relic_base_append_hashed()`, and the secondary
indexes where one is published or dropped, so that something is derived
exactly when the tuple total differs from the base total or an index
exists. The same count makes `relic_base_total()`, which
`relic_relations_current()` walks the base stores for on every query,
constant time.

**The arity-zero tuple convention is written twice.** [Low, component:
`lib/facts.c`, `lib/eval.c`] A tuple of no columns is passed as a pointer to
a static one-element array that is never read, in `set_tuple()` and in
`seed_member()`, and explained a third time in `tuple_append()`. One helper
in `lib/relation.c` returning a readable tuple pointer for any arity would
keep the convention in one place.

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

**The reason given for reading a load whole is out of date.** [Low,
component: `lib/parse.c`, `specs/design-parser.md`] `read_stream()`'s
comment, `specs/design-parser.md` section 8 and the item above say a stream
is read whole because the syntax tree a load produces is as large as its
input. A load's facts are staged rather than built into the tree
(`specs/design-facts.md`), so for a file of facts the tree is nearly empty
and the text, 24 bytes per fact by the facts-loading item under
Benchmarks, is held only for the lexer, which scans one contiguous range.
State the reason that holds, and decide whether the parse should release
the text before the commit.

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
distinct constants grows the symbol pool or the integer table by one entry
per distinct constant, and a session grows the symbol table by one entry per
line, because the tool names each line's query `<query N>` and the parser
interns the name. Nothing walks either table per answer, so the cost is
memory rather than time, and it is bounded by the distinct text seen. A
query's answer leaves nothing in the context's tables, per
`specs/design-query.md` section 9; these entries are what a query does
leave. A fix is either a source name that is not interned -- diagnostics
copy it -- or a per-query scope for the symbols and values only the query's
text mentions, which is a change to interning and wants a design note first.

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

**A later load re-evaluates the whole program.** [Low, component:
`lib/eval.c`] Any change to the program after an evaluation discards every
derived relation and computes the fixed point again from the facts, for
every program:
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
every derived relation first and evaluates again the components it reads
completely, so a caller running many bound queries over one program seeds
their facts and evaluates the negated part once per query. What that part
holds depends only on the program, so it could be kept until the program
changes, with only the generated relations and the answer discarded between
queries. `specs/design-magic.md` section 9.2 chose the simpler rule so that
what a context holds after a query depends on that query alone; measure a
REPL or embedder workload before changing it.

**A query can derive more through the rewrite than without it.** [Medium,
component: `lib/magic.c`] Each adornment of a predicate derives a relation of
its own, and the budget bounds the rules generated, not the tuples those
relations hold. Once any call reaches a predicate with nothing bound, every
other call of it reads that free relation, so `path(X, Y)?` holds `path`
once. A query can still reach one predicate with several bound patterns and
none free, whether or not the query binds a column itself: a ternary
predicate called with its first column bound and with its second derives
two relations, each of which may approach the whole of it. The general
remedy is for an adornment to read one that binds a subset of its columns; a
cap on adornments per predicate, falling back to the free one, would bound
the multiple. The rewrite is correct as it stands, and the tuples count
against `max_tuples` and `max_memory`.

**A predicate the first adornment search reads free stays free.** [Low,
component: `lib/magic.c`] The second search starts from every mark the first
made, including one only an adornment the second no longer reaches put
there, so the rewrite can compute a predicate in full that it then reads
only through bound calls. That needs a rule whose body the planner orders
differently for two bindings of its head. In `s(a, Y), s(U, V)?` over
`s(X, Y) :- g(Z), t(X, Z, Y).` and
`t(X, Z, Y) :- h(X, W), q(V, Y), k(Z, V).`,
with `k` large, the first search reaches `t` through `s$bf`, with `X` bound,
and calls `q` before `k`, free; the second reaches `t` only through `s$ff`,
with `Z` bound, calls `k` first and `q` bound, and still derives `q` whole.
The rewrite is correct and holds no more than the fixed point of `q`;
searching until the marks stop changing, with a mark cleared when nothing
the search keeps reaches the predicate free, would drop it.

**A rule holding its own head can leave a supplementary relation unread.**
[Low, component: `lib/magic.c`] A source rule with its head among its
positive literals derives nothing new. Drafted for the free adornment, with
the call on its head subsumed and a cut before that call, its adorned rule
reads its own head and is dropped, but the supplementary rule before it is
kept and evaluated with nothing reading it. Dropping such a source rule
before the rewrite drafts it would remove both.

**The rewrite's adornment tables are outside the memory account.** [Low,
component: `lib/magic.c`] The kept and subsumed adornments, the tables that
find them and the table of generated rules are grown with
`relic_grow_array()` rather than in the rewrite's charged arena, so
`max_memory` does not see them. They are bounded: each kept adornment counts
against the predicate limit or budget, each subsumed one was reached by a
call in a rule a search read for a kept adornment, and each generated rule
is charged to the rule limit and the literal budget. Growing them in the
charged arena, or charging the account for them, would make `max_memory`
count them too.

**The adornment search and the draft each walk a body.** [Medium, component:
`lib/magic.c`] The search for the adornments a query reaches
(`reach_calls_in_order()`) and the draft of the rewritten rules
(`rewrite_body()`) each plan a rule's body and walk it, and they must agree
on the rules walked, the order, the columns each call binds and which calls
read on demand. The only guard is the internal-error diagnostic
`adornment_of_call()` gives for a call the search never recorded, so an edit
to one walker would surface as a failed query on some program rather than as
a failing test. Recording, per adornment and rule the search walks, the
order and the calls it reached would let the draft replay the record by
construction, and let the second search reuse the first's plans rather than
plan each body again.

**Reading a query's reach completely is a mode beside the rewrite.** [Low,
component: `lib/magic.c`, `lib/eval.c`, `lib/explain.c`] When no adornment
the rewrite keeps binds a column, it generates nothing and sets
`reads_completely`, which evaluation and explain each test, and the
rewrite's query then aliases the caller's rule where it is otherwise an
arena copy. `read_reached_completely()` closes the reached components over
their dependencies with a closure of its own. Treating the reached
predicates as complete components from the start, as the negated part of a
query already is, would remove both branches and the second closure.

**The Magic limits count the adornment search.** [Low, component:
`lib/magic.c`] Each adornment search is held to `magic-preds`,
`magic-rules` and the literal budget on its own. The first search keeps
bound adornments of a predicate that the second drops once a call reads the
predicate free, and the rule limit counts every rule a search reads where
the draft counts only the rules it keeps, dropping duplicates and rules that
derive nothing. So a limit may refuse a query whose finished rewrite would
have fit, and a configured `magic-rules` can refuse a query the rewrite
without subsumption answered (`tests/lang/magic-rules-read.dl`); the
predicate limit and the literal budget cannot. Counting only what the draft
keeps needs the recorded walk of the item above, so that exploring is
bounded apart from what is kept.

**An abandoned rewrite repeats the complete part.** [Low, component:
`lib/eval.c`, `lib/explain.c`] A rewrite over its budget is abandoned after
the components it reads completely have been evaluated, and answering from
the whole program then discards them and evaluates them again. `-e` shows
both passes, each complete component printed twice with nothing between
them to say why; the warning goes to standard error. An explain line marking the abandoned
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

**The rule limit counts a body's demand calls twice.** [Low, component:
`lib/magic.c`] A body of k demand calls generates a magic rule and, after
the first, a supplementary rule per call, about 2k rules, and both count
against `max_magic_rules`. With `-l magic-rules=1000`,
`r(X) :- p(X), ..., p(X)` of 600 calls is refused, where writing the
prefixes out in full kept 602 rules and answered it, at 15 s and 5.9 GB. A
caller who sets the limit sized the rewrite, so the question is only what
a generated rule means to them: decide whether supplementary rules count.

**An index keeps a key's entry and its values in two arrays.** [Low,
component: `lib/index.c`] `entries` and `key_values` are both indexed by key
identifier, but each has its own capacity, growth, projection and overflow
check, and they stay in step because the reservation, the byte count, its
projection and the release each name both. One array holding each key's
entry followed by its `key_length` values would keep them in step by
construction, drop a capacity and an overflow clause, and put a key's values
on the cache line `relic_index_first()` reads right after comparing them.

**No test forces a tag collision in an index's key table.** [Low, component:
`tests/unit/test_index.c`] The key table is tagged, so a probe is handed
another key's candidate about one time in 256 rather than on every slot it
passes, and the late-key test's forty keys almost never reach one: comparing
too few words of a key does not fail it, though the larger tests catch it.
Thousands of keys, or a test-only way to place a key under a chosen hash,
would make the key comparison the thing that test checks.

**Append-only invariant versus incremental maintenance.** [Medium, component:
`lib/relation.c`] D-10 represents a delta as a range of tuple identifiers,
which requires that nothing compacts, deletes or renumbers a relation during a
fixed point. The store holds to it by construction: nothing removes, reorders
or renumbers one tuple, and the one removal there is, discarding every
derived relation at once, runs only between evaluations, when no range is
live. The debt is the moment that stops being enough. Any incremental
insertion or DRed-style deletion support has to confront it first: a range
captured before such an operation means something else afterwards, and
nothing in the range would say so. A load's commit and `relic_fact_add()`
need no more than the existing discard: they append to base stores only
between evaluations, after discarding what was derived, and only the
evaluator inserts into derived relations, which is what makes every derived
tuple re-derivable and the discard safe. `specs/design-relations.md`
section 2 states it.

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

**No fuzzer reads the lexer's input to its exact end.** [Low, component:
`tests/fuzz/`, `lib/lex.c`] The lexer reads nothing past its byte range,
and the unit tests hand it every token class in an allocation of exactly
its length, so AddressSanitizer catches a scanner that reads one byte too
far. The fuzz targets reach it through `relic_load_file()`, whose buffer has
spare capacity past the text, and through `relic_load_string()`, whose text
is terminated, so a fuzzer cannot see such a read. A target that hands
libFuzzer's exact-size data to the lexer would.

**A carriage return after a closing quote is "inside a field".** [Low,
component: `lib/import.c`] In csv, `"a"` followed by a carriage return that
no line feed follows is reported as `carriage return inside a field`, the
message of section 7 of `specs/design-import.md` for a stray carriage
return, though the field has closed. The alternative is the message for a
byte after a closing quote, `found 0x0d`. Either is defensible; decide
whether the table in section 7 should say which.

**A load or import can fail after its facts are committed.** [Low,
component: `lib/import.c`, `lib/parse.c`] `relic_import_path()` and
`relic_load_path()` commit what they read and then close the file, and a
close that fails turns the call into a failure with its facts already
added, against the promise that a failed call adds nothing. A read-only
close fails only on a network or user-space file system. Close before
committing, or do not count a close failure after the whole stream was
read.

**The tool repeats the library's import rules.** [Low, component:
`cmd/relic/import.c`] `-I` checks the predicate name and the 32-column arity
bound itself, so that a malformed operand is a usage error, status 2, rather
than a failed load, status 1. The rules match the library's today; if the
library widens either, the tool refuses imports it would accept. Export the
arity bound and a name check in `<relic.h>`, or give a refusal a class the
tool can map to status 2.

**The terminal refusals have no test.** [Low, component:
`cmd/relic/main.c`, `tests/cli.sh`] A program or an import of `-` read from
a terminal is a usage error, but no test runs the tool on a pseudo-terminal,
so neither branch of `refuse_terminal_input()` is exercised. `script(1)`
takes different arguments on BSD and Linux; a small pty helper under
`tests/` would serve both.

**A deferred round costs more than a serial one on one thread.** [Medium,
component: `lib/task.c`, `lib/round.c`] Under `-X parallel`, in one build
and with identical counters, `tc-cyclic` takes 2.62 s against 2.24 s,
`cspa` 0.21 s against 0.19 s, and `chain-closure` 0.61 s against 0.42 s.
Screening a batch probes the store as inserting it does, and a tuple the
store lacks then pays for the worker's screen and candidate buffer and is
probed once more in the merge; the screen starts empty every round and is
rehashed at each doubling. That overhead is the floor under every gain the
pool can give, and it is what the threshold of
`specs/design-parallel.md` section 5.4 must be measured against. Reserving
the screen from what the round's items estimate, rather than growing it
from nothing, is the first thing to try.

**The pool's spin bound counts iterations, not time.** [Low, component:
`lib/pool.c`] `RELIC_SPIN_LIMIT` pauses 4096 times before a waiting worker
parks, which was sized on x86, where `pause` takes tens to a hundred
cycles; `yield` on aarch64 is close to free, so a worker there parks
within microseconds and a component of many small rounds pays a wake-up
per round. A bound in time, the clock read every few hundred pauses,
would hold on every processor. Unmeasured on aarch64.

**The words workers share are not on cache lines of their own.** [Low,
component: `lib/round.h`] `specs/design-parallel.md` section 4.3 puts the
job's claim, abandon and completion words and each worker's counters on
lines of their own; the job's words share a line with fields every worker
reads, and `relic_worker_t` is not padded, so one worker's counters sit
beside the next one's scratch. Padding the workers to 128 bytes measured
0.778 s against 0.789 s on `tc-cyclic` at sixteen threads, within noise.
The workers are allocated with `calloc()`, which does not honour an
alignment above `max_align_t`.

**A fork descendant that reuses the pool's process identifier.** [Low,
component: `lib/pool.c`] A pool knows it was carried into a child of
`fork(2)` because the process identifier differs from its creator's. A
descendant several forks down whose identifier happens to equal the
exited creator's would take the threads for its own and wait for them.

**The eval fuzz target defers on the calling thread only.** [Low,
component: `tests/fuzz/fuzz_eval.c`] Its deferred context has one thread,
so the corpus replay and `make fuzz` hold the merge of one worker's
candidates to the serial answers but never several workers'; the random
suite and `tests/langtest.sh` do. Giving it a pool costs a thread start
per input.

## Benchmarks

**`bench/run.sh` has no thread count.** [Low, component: `bench/`]
`specs/design-parallel.md` section 16 asks for a column giving the thread
count and a run at sixteen in `bench/baseline.txt`, so that a regression
in the speed-up is visible where every other performance regression is.

**`bench/compare/run.sh` compares on one thread only.** [Low, component:
`bench/compare/run.sh`] The harness runs every engine on one thread, Souffle
with `-j1` and DuckDB with `threads = 1`, so nothing records how relic's
speed-up at `-j 16` compares. A threaded column needs a Souffle built with
OpenMP, since the one on this machine's `PATH` is not and ignores `-j`, and
its programs translated with `-j16`, since a compiled Souffle program has
parallel loops only when translated with `-j`. `docs/todo-parallel.md`
records one such comparison made by hand.

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
Profiles of `tc-cyclic` and `sg-cyclic` put 59 and 49 per cent of the time in
the duplicate check of relation insertion, with batched insertion overlapping
its misses (`specs/design-evaluation-cost.md` section 6); it held 73 and 68
per cent before. 92 to 96 per cent of those checks find a duplicate, each a
miss on the slot array and a miss on the tuple that confirms it; a table of a
million tuples does not fit in cache. Souffle keeps the same relation in a
B-tree and inserts with the outer loop fixed on the head's first column, so
consecutive inserts land in the same leaf. A hash stored beside each
identifier was measured and set aside (`specs/design-evaluation-cost.md`
section 5.2): it saves the tuple reads of the candidates that are not the
match, which a duplicate-heavy workload makes few of, and took `tc-cyclic` and
`sg-cyclic` 8 per cent faster and 4 per cent slower for a larger peak on both.
Worth measuring next: the stored hash again with batching in place, and
whether a successful search visits enough slots past the first to make
prefetching them pay.

**A growth's slack and transient keep the peak above the live heap.**
[Medium, component: `lib/util.c`] Mapped growth releases every outgrown
generation, but `chain-closure` still peaks at 39.4 MB against a live heap
of 33.6 MB, 17 per cent over the 10 per cent that
`specs/design-comparison-cost.md` section 10 gates step 5 on. Two causes
remain. An identifier table's capacity is a power of two because a probe
masks the hash, and a column array doubles because one policy serves every
arity, so a relation just past a doubling is half empty on both. And a
table's growth holds both generations of its slots while it rehashes, one
and a half times the new size at that moment. Growth by three halves under
a multiply-shift range reduction, `(hash * capacity) >> 32`, would keep a
mask-free probe and halve the slack; rehashing a mapped table within its
own extended mapping would remove the transient where the platform can
extend one. Measure both on `chain-closure` and `facts-join` before
changing either policy. The memory limit does not count the transient
either: an insertion or a round's merge admitted at the limit holds one
array's or one table's old generation beside the new one while it is
copied, one at a time.

**A mapped array's growth always copies.** [Low, component: `lib/util.c`]
Growing a mapped array maps the new size, copies and unmaps the old. glibc's
`realloc()` of a large block moved it with `mremap(2)` without copying, so on
Linux the copy is new cost on the largest arrays. A `configure`-selected
`mremap()` would remove it there; not yet measured.

**A table whose slots were not zeroed would hang rather than fail.** [Low,
component: `lib/util.c`] A probe stops at the first empty slot, and nothing
bounds it by the capacity, so a table allocated without zeroing probes a
full table of garbage forever instead of failing a test. Every slot and tag
array is zeroed where it is allocated, and the large-table test checks it
for a mapping; a debug-build bound on a probe's length would turn such a
defect into an assertion.

**The allocation-failure suite's renaming of `mmap` may miss on 32-bit
glibc.** [Low, component: `Makefile.am`, `tests/unit/test_exhaustion.c`]
With `_FILE_OFFSET_BITS=64`, glibc declares `mmap` with an assembler label
naming `mmap64`. The suite's `-Dmmap=relic_test_mmap` renames that
declaration too, so the hook would carry the label and the library would
call the system's `mmap64` directly. `tests/allocators.sh` reports `mmap64`
as an allocator, so the gap fails the build rather than hiding; the fix is
to rename through a wrapper the library calls instead of the system name.
Settle when a 32-bit glibc job exists.

**Importing facts misses the comparison's gate.** [Medium, component:
`lib/symbol.c`, `lib/value.c`, `lib/facts.c`, `lib/import.c`,
`bench/compare/facts-join`] `specs/design-comparison-cost.md` section 10
gates its step 6 on `facts-join` at scale 10, three million ternary rows
naming three million distinct symbols, under 2 s and 120 MB. With its
section 7 in place the import and query take 2.9 to 3.2 s and 218 to 220 MiB
of resident peak, from 4.2 to 4.6 s and 289 to 292 MiB. The peak is the
import's commit, which the import alone brings to 218 MiB, because three
things are held at once there. The staged facts hold about 68 MiB: their
tuples, hashes and marks, and the table that finds them. The symbols hold
about 95 MiB: three million sixteen-byte entries, 46 MiB resident of a 64
MiB array; their packed bytes, 25 MiB; the tagged slots, 20 MiB; the byte
per symbol recording which types use it, 4 MiB. The base store the commit
appends to holds 50 MiB. Two thirds of the time is the import: interning the
symbols (27 per cent), the staging set (17) and appending to the base store
and its canonical index (7), each probe a cache miss into a table of
millions. The query is the rest, a third to a half of it building the join's
index by two profiles taken under load. Candidates, none designed: for a
load into an empty store, handing the staged facts to the store rather than
copying them, so that the peak holds them once rather than twice; an entry
of a 32-bit offset into the string arena and a length, eight bytes rather
than sixteen, about 23 MiB; growing the pool by walking the entries in
identifier order and placing each by its stored hash; and checking an
imported symbol field once, where the import checks it is a symbol and
interning the value checks again. Loading the same facts as source text also
holds the file's text whole until the commit, 24 bytes a fact, though
nothing reads it once the parse ends.

**The hashed commit trusts two copies of the seed.** [Low, component:
`lib/facts.c`, `lib/relation.c`] A staging set hashes a fact with its own
copy of the context's seed, and the commit places the fact in the base
store's canonical index by that hash, which is right only while the set's
copy and the store's copy are equal and both hash through
`relic_tuple_hash()`. Both are copied from the context when they are
created and never change, and a debug build asserts the hash matches; a
release build trusts it. Reading the seed from the context in the staging
set, or passing a tuple hash of a type only `relic_tuple_hash()` produces,
would make the agreement hold by construction.

**A load that fails to parse still stages and interns the facts it read.**
[Low, component: `lib/parse.c`] A fact is staged, and its predicate
interned, as it is read, so the facts before a syntax error name predicates
that stay interned, and the facts after it are staged and thrown away. The
design accepts the identifiers (`specs/design-facts.md` section 12), but
they are observable: a later load's components are numbered after them, so
`-e` lists them in another order than a fresh context would. Staging after
the first error also costs memory in proportion to the text until the load
returns. Stopping the staging once the load is certain to fail would bound
the second; only interning at the commit would remove the first.

**A body atom reaches the tree through the body array's one writer.** [Low,
component: `lib/parse.c`] A head's arguments are copied into the tree by
the statement append itself, but a body's are copied by `body_append()`,
and the statement append trusts that its `literals` came from there.
Collecting every argument of a statement in one growable scratch array, and
copying head and body in the statement append, would make that append the
only way into the tree and cost one arena allocation per statement rather
than one per atom.

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

**Mapped arrays are outside AddressSanitizer's quarantine and leak check.**
[Low, component: `lib/util.c`] A growable array of a mebibyte or more lives
in a mapping of its own. The sanitizer reports a write just outside it,
since the header before the first item and the bytes past the last are
marked unaddressable, but a stale pointer into a mapping already released
reads whatever is mapped there next instead of being reported as a use
after free, and a mapping never released is not reported as a leak. The
evaluator holds no pointer into a relation across a growth, which is what
such a stale pointer would need. A sanitizer build that kept every array in
the heap would catch both, at the cost of not exercising the mapped path
under the sanitizer.

**Files a load or import opens are inherited by child processes.** [Low,
component: `lib/parse.c`, `lib/import.c`] `relic_load_path()` and
`relic_import_path()` open with `fopen(path, "r")`, without close-on-exec, so
a multithreaded embedder that forks and executes while one is open passes
the descriptor on. `"re"` asks for close-on-exec on FreeBSD, glibc and
current macOS; confirm every supported platform accepts it, or open with
`open(2)` and `O_CLOEXEC` and `fdopen(3)`, before changing both.

**One field of an imported table can grow without bound.** [Low,
component: `lib/import.c`] The reader holds a field whole before typing it,
and its buffer is not charged to `max_memory`, so a hostile table with one
unterminated csv quote makes the reader hold the rest of the input, however
long. `<relic.h>` says the field is not counted. Charge the buffer to the
limit, or bound a field's length by a limit of its own.

**A long-lived context gives a timing oracle on its hash seed.** [Low,
component: `lib/util.c`] The seed defeats crafted collisions because an
author cannot learn it, and a batch run gives nothing to learn from. A REPL
session (`-i`), or an embedding that feeds one context facts from an
untrusted party over time, lets that party time its own insertions against
one fixed seed and, in principle, search for keys that share a cluster.
No attack has been built. Decide whether a context used this way should be
documented as trusting its input, or whether a stronger keyed hash for the
byte strings the symbol table interns is worth its cost there.

**The arity-limit message quotes a predicate name of any length.** [Low,
component: `lib/parse.c`] `predicate has more than 32 arguments` names the
predicate through `predicate_name()`, which is not clamped to
`RELIC_QUOTED_TEXT_MAX` as quoted token text is, so a 500-byte name makes a
624-byte message. The bytes are identifier characters and nothing leaks, but
a message should name its subject in a bounded length.

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

**A load or import refused by a limit stages and interns its whole input
first.** [Medium, component: `lib/facts.c`, `specs/design-facts.md`,
`specs/design-import.md`] The commit measures staged tuples against the
base stores only once the input has been read, and checks the tuple and
memory limits only then (`bases_fit()`). So a table or fact file of a
billion distinct rows under `-l tuples=1000000` is staged and interned in
full before it is refused, and nothing it interned is released. For an
embedder feeding one long-lived context untrusted tables, each refused
import permanently spends value identifiers. Nothing can change a base
store while the context is busy, so staging could mark each tuple new or
not as it stages it -- the commit's measure step, moved earlier -- and
refuse as soon as the base stores' tuples plus the new ones exceed
`max_tuples` (`relic_base_total()` gives the first term), or their
projected bytes exceed `max_memory`. That bounds staging and interning by
the limits, by construction. A load would still read its text whole before
staging any of it; an import reads a record at a time and would stop
reading too. Still to settle is the message of a refusal part way through a
load naming several predicates: it names the first predicate in load order
that would grow, which a refusal before the end of the load cannot yet
know.

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

**Language and tooling gaps.** [Medium] What Souffle, LogiQL, Nemo, clingo,
CodeQL, DDlog and the tabling Prologs offer that relic does not, sorted in
`docs/todo-language.md` into what is worth adding -- string operations and
provenance first, then disjunction, file inclusion,
constraints, relation export and per-rule profiling -- what is worth
measuring before deciding, and what stays out on purpose with the reason.
Each item names the engines that have it and what a relic program does
without it; the verdicts are provisional until a design exists.

* string operations, specified in `specs/design-strings.md`: `length`,
  `cat`, `substr`, `index`, `match` and `str` as calls inside expressions,
  tests as comparisons over the positions they return, a regular expression
  matcher of relic's own in `lib/regex.c`, computed strings on the
  pending-value mechanism with the symbol pool charged to the memory
  limit; five steps, of which the front end is built and the planner
  refuses a rule holding a call until the executor evaluates one
* relation export, in `docs/todo-language.md`
* incremental insertion maintenance
* DRed-style deletion maintenance
* parallel SCC evaluation, specified in `specs/design-parallel.md`: a
  worker pool the context owns, semi-naive rounds partitioned over a
  frozen store and merged in the serial order so that output and counters
  are identical at every thread count, then a striped merge, then a
  parallel load; six steps, each gated by a measurement, of which the
  executor scratch, deferred rounds on one thread and the pool are built
  and the parallel merge, the parallel load and components side by side
  remain
* persistent compiled rule plans
* better cardinality estimation
* further Magic Sets and negation optimization
