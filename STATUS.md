# relic status

A snapshot of what exists. Not a roadmap: for sequencing see `relic-plan.md`,
for open work see `TODO.md`, for the target design see `specs/architecture.md`.

## Implemented

**The build skeleton, stage S0, milestone M0.** The tree configures, builds,
tests, installs and distributes on macOS; `relic` prints its version and
rejects anything else as a usage error.

| Component | State |
| --- | --- |
| `configure.ac`, `Makefile.am`, `autogen.sh` | autoconf 2.69, automake 1.13, libtool 2.4.2 or newer; one non-recursive `Makefile.am` |
| `m4/` | five local macros: C11 dialect, compiler flags, sanitizers, libedit, debug and assertion options |
| `cmd/relic/main.c` | `-V` only; unknown options and operands are usage errors |
| `compat/` | `compat.h` and replacements for `getentropy`, `reallocarray`, `strlcat`, `strlcpy`, `strtonum`, each linked only where the platform lacks it and compiled everywhere by the test suite |
| `librelic.pc.in` | pkg-config module `librelic` |

**The foundations, stage S1.** The storage and reporting primitives every
later stage sits on, specified in `specs/design-memory.md`. No Datalog yet.

| Component | State |
| --- | --- |
| `lib/arena.c` | chunked region allocator; sticky failure flag, cleared by a reset and by the context-arena funnel |
| `lib/util.{h,c}` | `relic_grow_array()` over `reallocarray()`; one open-addressing identifier table, reserved and placed as separate steps; hashes keyed by a 64-bit seed passed to every call, a keyed FNV-1a fold and a finalizer, an integer's high half finalized before its low half joins it, and a zero seed asserted against |
| `lib/symbol.c` | byte strings interned to 32-bit identifiers; bytes in the context arena, embedded NULs preserved |
| `lib/value.c` | the value table and `relic_dom_t` of D-3; interning for all three types, the language order of section 10.1 and the total order of section 61 |
| `lib/diag.c` | diagnostic callback, last-error buffer, and a stderr formatter that escapes non-printable bytes |
| `lib/api.c` | context lifecycle and the context-arena allocation funnel; each context draws a nonzero hash seed from `getentropy(2)`, copied by every table owner -- symbols, values, predicates, relations and their indexes, the Magic rewrite -- and creation fails without one |
| `include/relic.h` | `relic_t`, `relic_value_t`, `relic_location_t`, `relic_config_t`, the diagnostic callback, `relic_create()`, `relic_create_config()`, `relic_destroy()`, `relic_diag_set()`, `relic_diag_stderr()`, `relic_error()`, `relic_string()` |
| `compat/queue.h` | bundled OpenBSD copy, provenance and patches in `compat/README.md` |
| `tests/` | the unit suites for each module, plus the tree checks |

A configuration is accepted and stored but not enforced: nothing allocates
relation or index storage for a limit to apply to yet, and no `flags` value is
defined. The header says so. `compat/tree.h` is not bundled, because no
subsystem calls for a red-black tree yet.

**The front end, stage S2.** Source text becomes an abstract syntax tree and
bad source text becomes diagnostics, specified in `specs/design-parser.md`.
`relic -c` reads files or standard input, reports what is wrong with them and
evaluates nothing.

| Component | State |
| --- | --- |
| `lib/lex.c` | byte-oriented lexer over its own classification table; every token class of architecture section 13, string escapes including `\xHH`, `strtoll` integer literals with a range check, `%` comments, line and column on every token |
| `lib/parse.c` | recursive descent over the grammar; error recovery synchronizing on `.` and `?` with one diagnostic per statement; the loading entry points and the whole-stream reader |
| `lib/ast.h` | terms, atoms, literals, statements and the program list; a fact is a rule with an empty body |
| `include/relic.h` | adds `relic_load_file()`, `relic_load_path()`, `relic_load_string()` |
| `cmd/relic/main.c` | `-c` with file operands and standard input; `-V` unchanged |
| `tests/lang/` | golden language tests run by `tests/langtest.sh` as automake's `DL_LOG_COMPILER`, with `tests/langfiles.sh` failing the build if one is not registered |

Constants are interned as they are parsed, so a term holds the same domain
identifier a tuple column will hold. `not` is the negation keyword only where
a literal may begin, so `status(foo, not).` is an ordinary fact. A load is all
or nothing: a program never holds half of a file.

A bare `relic file.dl` is a usage error, because evaluating a program is what
it will mean and checking is not.

**The program model, stage S3.** The syntax tree becomes a validated program,
specified in `specs/design-program.md`. `relic -c` now checks what it loads.

| Component | State |
| --- | --- |
| `lib/pred.c` | predicates interned by `(name, arity)` through the same identifier table the symbol and value pools use; first-mention location, definedness, and the undefined-predicate sweep |
| `lib/rule.c` | rule compilation in one pass: predicate resolution, variable numbering into the slots of architecture section 25, the range-restriction check, the singleton warning, and `relic_check()` |
| `lib/program.h` | the compiled forms: arguments, atoms, comparisons, literals, variables, rules, and the program that holds them |
| `include/relic.h` | adds `relic_check()` |
| `cmd/relic/main.c` | `-c` validates after loading, and skips validation when a load failed |
| `tests/lang/` | twelve more golden tests: the accepted and rejected examples of architecture sections 10 and 14, both variable-limit boundaries, predicate identity, singletons with their exemptions, the undefined-predicate warning, and the architecture section 72 program |

Safety is one set computation, not a search: a rule is safe when the variables
of its head, its negated literals and its comparisons all occur in a positive
relational body literal. `_` is a wildcard in a positive body literal and takes
no slot; anywhere else it can never be bound and the rule is rejected. Checking
is incremental -- a cursor records the last statement compiled, so a check
compiles only what was loaded since the previous one, reports each diagnostic
once, and returns its previous answer when there is nothing new. A program that
has ever held a rejected rule stays refused.

**The relation store, stage S4.** Somewhere to put tuples, specified in
`specs/design-relations.md`.

| Component | State |
| --- | --- |
| `lib/relation.c` | the dense tuple array with the arity as its stride, the canonical full-tuple hash index, `relic_relation_insert()` returning newness, membership, reading a tuple by identifier, iteration by identifier range, and the tuple and memory limits |
| `lib/relation.h` | the tuple identifier, the half-open tuple range, the relation and the table of pointers to them |
| `lib/util.{h,c}` | adds `relic_hash_words()`, and the two pure projections the memory limit needs: the capacity a growable array would reach and the bytes an identifier table would hold after a reservation |
| `include/relic.h` | unchanged; `max_tuples` and `max_memory` are now enforced, and the header no longer claims otherwise |
| `tests/unit/test_relation.c` | the set property, growth, ranges and deltas, arity 0 and arity 32, relation ownership, both limits with their golden diagnostics, and a million tuples inside a memory envelope |

A relation is a set, and that is the property everything above it rests on: an
insertion reports newness, which is how a fixed point is detected. The store
is append-only by construction rather than by rule -- it offers no operation
that removes, reorders or renumbers one tuple, and from S8 the only removal is
a discard of every relation at once, between evaluations -- which is what lets
a semi-naive delta be a range of identifiers instead of a relation of its own.

A relation never moves. The table holds pointers to individually allocated
relations, so creating one cannot invalidate another, and a plan or an
evaluation may resolve a relation once and hold it. Storing the relations
inline was tried first and was wrong: it made every holder responsible for not
keeping a pointer across a call that might create a relation, and the first
thing that list cost was a use-after-free at the ninth predicate.

Arity zero needs no case of its own: the hash of an empty key is defined and
the comparison of two empty keys is true, so a relation of arity zero holds
zero or one tuple through the ordinary path.

Both limits are checked against what a relation would hold after an insertion
and before anything is modified, so a refused insertion leaves the relation
exactly as it was. The memory limit counts allocated capacity rather than the
smaller number a tuple count implies, since a limit that ignored the
difference would allow twice what was asked for.

**The plan, the executor and the tool, stage S5, milestone M1.** Positive
recursive Datalog answers queries from the command line, specified in
`specs/design-plan.md`, `specs/design-eval.md` and `specs/design-cli.md`.
`relic -q 'path(a, X)?' graph.dl` prints `b`, `c` and `d`.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | the plan representation of D-8 and the textual planner: positive literals in written order, each a `SCAN` or, when every argument is bound, a `MEMBER`; each comparison a `FILTER` at the earliest point its operands are bound; the head an `EMIT`; a plan is refused unless every body literal became exactly one operation |
| `lib/eval.c` | the executor, one loop over a level and a resume flag with no recursion; the naive fixed point of D-9; `relic_materialize()` and `relic_query_run()` |
| `lib/rule.c` | a query compiles into a rule whose head names no predicate: its named variables in first-occurrence order are the columns, and safety is the ordinary check of a head; `relic_query_prepare()`, `relic_query_count()`, `relic_query_loaded()`, `relic_query_destroy()` |
| `lib/parse.c` | parses into any arena, so a prepared query owns its syntax tree; `relic_parse_query()` holds query text to exactly one query |
| `lib/value.c` | one definition of the total order of section 61, reached by domain identifier and, as `relic_value_compare()`, by public value |
| `include/relic.h` | adds `relic_materialize()`, `relic_query_t`, `relic_row_cb`, the five query functions and `relic_value_compare()` |
| `cmd/relic/main.c` | the phases of `design-cli.md`: load every operand or standard input, check, prepare every query, then evaluate; `-a`, `-c`, `-F`, `-o`, `-q`, `-s`, `-V`, and `-X` from S7; each `-q` is named `<query N>` in diagnostics |
| `cmd/relic/output.c` | text, csv, tsv and json, byte for byte as `design-cli.md` section 4 specifies, a string literal escaping C0 and C1 controls and bytes that are not UTF-8; `-s` through a merge sort over the library's total order |
| `lib/pred.c` | no predicate may have more than 32 arguments, enforced at interning, the one path every predicate takes |
| `tests/` | `test_plan`, `test_eval` and `test_query`; an evaluation sweep in `test_exhaustion`; `tests/cli.sh` for standard input, `-o` and write failures; twenty-two golden language tests covering every binding pattern, recursion, comparisons across types, all four formats, sorting, and every query diagnostic |

The queries a program contains are now compiled and checked by
`relic_check()`, so an unsafe query makes the program invalid exactly as an
unsafe rule does. With `-q`, only the command line's queries run; the
program's own are still checked.

A query is answered by running its rule once into its own answer relation,
then streaming that relation. No row is delivered until the answer is complete, so
a query that exceeds a limit delivers none. The relations record how many
rules they are a fixed point of, so a query over a materialized program after
nothing new was loaded evaluates nothing.

**The dependency graph and stratification, stage S6.** The check knows what
depends on what and rejects recursion through negation; evaluation runs in the
order that analysis settles, specified in `specs/design-stratify.md`.

| Component | State |
| --- | --- |
| `lib/depgraph.{h,c}` | the predicate graph in compressed adjacency form, one edge per relational body literal in rule and literal order, each remembering its rule, literal and sign; strongly connected components by an iterative Tarjan search, completed dependencies first |
| `lib/stratify.c` | least strata over the condensation; one error per component holding a negative edge, naming the shortest cycle through it with a note at the literal of each edge; the schedule of components with rules, grouped by stratum, cached on the program until a check compiles another rule |
| `lib/eval.c` | materialization plans every scheduled rule, then runs the schedule component by component: the naive fixed point for a recursive component, one pass for any other; each plan owns its cursors |
| `lib/rule.c` | `relic_check()` runs the analysis after compiling and before the undefined-predicate sweep |
| `lib/arena.{h,c}` | the arena declarations in a header of their own, so the program model can hold one, with moving an arena by assignment stated as part of its contract; `relic_arena_allocarray()` refuses a size that overflows |
| `tests/` | `test_stratify` for component order, strata, rule grouping, independence of textual order, rejection, and a chain and a cycle of a hundred thousand predicates analysed on a one-megabyte stack; two sweeps in `test_exhaustion`, over rejecting a program and over replacing a schedule; six golden tests, reproducing architecture sections 9 and 30 exactly and fixing which cycle is named and in what order |

A program found unstratified stays refused and is not analysed again, so a
second, independent cycle introduced by a later load is not reported until
the first is gone. The command-line tool loads every operand before it checks
and reports every cycle in one run.

**Semi-naive evaluation, stage S7, milestone M2.** Recursion no longer redoes
the work it has already done, specified in `specs/design-eval.md` section 4
and `specs/design-plan.md` section 5.3.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | `relic_plan_rule()` takes a delta literal, which it places first as a `SCAN` marked as reading the delta, even when every argument is a constant; any other literal is refused as an internal error |
| `lib/eval.c` | each component is planned with its members' relations, one plan per rule, and for a recursive component under the default strategy one delta variant per positive literal over a member; the semi-naive fixed point runs every plan once, then the variants round after round, each member's delta the identifier range it gained in the round before, until a round adds nothing; the naive fixed point stays behind `RELIC_F_NAIVE`; the executor counts iterations, derivations, insertions and scanned tuples |
| `include/relic.h` | `RELIC_F_NAIVE`; `relic_stats_t` and `relic_stats()`, the cumulative counters and a snapshot of tuples, storage and values by type |
| `cmd/relic/main.c` | `-X naive` and `-X stats`, the counters printed to standard error when the run ends, whether or not it succeeded; any other `-X` name is a usage error |
| `tests/` | `tests/equiv/strategies.sh` runs every registered language test under `-X naive` and compares sorted output, diagnostics and status with the default; `test_eval` runs every answer scenario under both strategies and pins both strategies' counters on a chain, computed by hand; `test_plan` checks variant plans and refusals; golden tests for transitive closure over a chain, a cycle, a complete graph and a disconnected one, for rules with two and three recursive literals, and for the counters of both strategies |

The two strategies take the same number of rounds on a chain; what differs is
the work. Over a six-edge chain the default derives 38 tuples and scans 216,
the naive fixed point 118 and 694, and the golden counters fail if the default
regresses toward the naive figures.

**Negation execution, stage S8.** `not` works, specified in
`specs/design-plan.md` sections 3 and 5 and `specs/design-eval.md` sections 5
and 6. The architecture section 72 program evaluates correctly under both
strategies.

| Component | State |
| --- | --- |
| `lib/plan.{h,c}` | `RELIC_OP_ANTI_MEMBER`; a comparison and a negated literal are both tests, each placed at the earliest point every slot it reads is bound, in written order among tests placeable at one point; a negated literal is never a delta literal; the negation refusal is gone |
| `lib/eval.c` | the executor runs `ANTI_MEMBER` as `MEMBER` inverted; a changed program discards every relation and is evaluated again from nothing, for every program rather than only one that negates; the answer stream stops at an identifier its relation no longer has, so a callback that breaks its contract by loading cannot make it read past a relation |
| `lib/relation.c` | `relic_relation_discard_all()` empties every relation in place and returns both limit totals to zero; relations do not move |
| `include/relic.h` | `relic_materialize()` documents the discard |
| `cmd/relic/main.c` | `-l key=value`, repeatable, brought forward from S12: `tuples`, `memory`, `magic-preds` and `magic-rules`, the last two enforced from S10; values through `strtonum()`; a malformed `-l` is a usage error naming what is wrong |
| `tests/` | `test_plan` for where an `ANTI_MEMBER` goes and what its columns read, and a negated literal refused as a delta literal; `test_eval`, under both strategies, for the task graph of section 72 with negation stacked across strata, a negating query, arity zero, constants and a negation written before its binder, a later load that makes a derived tuple false, a discard that gives back what the limits count, and a contract-breaking callback; the evaluation sweep in `test_exhaustion` now includes a negated stratum; `tests/cli.sh` fails queries on `-l` limits and rejects thirteen malformed ones, a sign or a space among them; four golden tests, all compared across strategies by `tests/equiv/strategies.sh` |

A negated literal and a comparison go wherever their variables are first
bound, so `not done(a)` and `not frozen` are tested before any scan and reject
a rule's every binding at no cost.

**Indexes, statistics, planner and explain, stage S9, milestone M3.** A
literal with bound arguments is looked up rather than scanned, and a body is
ordered by what the relations hold, specified in `specs/design-index.md` and
`specs/design-planner.md`.

| Component | State |
| --- | --- |
| `lib/index.c` | secondary indexes: a key table of distinct keys, each key's first and last tuple, and a `next` identifier per tuple threading every key's chain in identifier order; reserve and add as separate steps, with an exact projection of what a reservation allocates; a lookup takes a probe laid out as a tuple, so only the index knows how a key is packed |
| `lib/relation.{h,c}` | a relation's indexes, at most one per key mask, at positions fixed until the next discard; every insertion reserves in every index before storing anything; `relic_relation_index()` builds an index over the tuples held, one tuple at a time under the memory limit, and publishes it only once complete; a refused build reports `limit exceeded: memory=N while indexing p/n` and leaves no index and no bytes behind; `relic_relation_distinct()` counts a column's distinct values with a scratch table and caches them until the tuple count changes; a discard drops every index and clears the cache, so an index an earlier program asked for costs a later one nothing |
| `lib/plan.{h,c}` | `RELIC_OP_LOOKUP`, keyed on exactly a literal's bound columns; greedy ordering by bound arguments, then the integer estimate, then source position; relations of the component being planned assumed to hold 1000 tuples, narrowed tenfold per bound column; `RELIC_F_NO_INDEX` and `RELIC_F_NO_REORDER` select the textual planner's choices |
| `lib/eval.c` | each component is planned when it starts, with its members marked as still being derived; a lookup walks its key's chain and stops at the relation's end when it started, as a scan does; the counter of tuples looked up |
| `lib/explain.c` | `relic_explain_set()`: each component's heading, fact count, rules, plans and delta variants as it is planned, and each query's plan before it runs; printable ASCII in a fixed 1024-byte line, cut with `...` |
| `include/relic.h` | `RELIC_F_NO_INDEX`, `RELIC_F_NO_REORDER`, `relic_explain_cb`, `relic_explain_set()`, `tuples_looked_up` |
| `cmd/relic/main.c` | `-e`, plans to standard error; `-X no-index` and `-X no-reorder`; `stats: tuples looked up` |
| `tests/` | `test_index` for chains against a scan under one- and two-column keys, a late build against one kept up, a walk while its chain grows, a discard dropping every index, exact accounting, the memory limit under 200 allowances as two indexes grow, refusing exactly the insertions that would pass it, a refused build, key validation and the distinct-count cache across a discard and refill; `test_plan` for the access path of all eight binding patterns of a 3-ary literal, the architecture section 23 ordering, a bound argument outranking a smaller estimate, selectivity, an empty relation first, the assumed size of a relation being derived and its division per bound column, and both reference switches; `test_eval` runs every answer scenario under both references as well as both fixed points, adds same generation, pins both planners' counters, and pins the counters of a lookup whose own rule extends the chain it walks; an index sweep in `test_exhaustion`; `tests/equiv/strategies.sh` compares every language test under `no-index` and `no-reorder` too; four `-e` golden tests, among them every byte class a string escapes and a line of exactly the longest length that is not cut; `tests/cli.sh` for `-e`, the references and a refused index build; `tests/perf.sh`, reachability over a million edges, bounded by the tuples read |

The default planner closes the six-edge chain of the S7 counters in two
rounds, deriving 48 tuples while scanning 33 and looking up 36; those golden
tests now pin the textual planner, and `stats-indexed` the default.
Reachability over the million-edge graph takes about four seconds, eight
rounds and a million lookups; without indexes it does not finish in the
test's budget. Most of the memory it takes, about 450 MB, goes to the fact
rules rather than the relations, which `TODO.md` records.

The stage is about 2,200 lines of library and tool code, comments included,
against a budget of 1050; `lib/explain.c` accounts for 650 of them.

**Magic Sets, stage S10, milestone M4.** A bound query derives only what its
bindings reach, specified in `specs/design-magic.md`. `relic -e -q
'path(a, X)?' graph.dl` shows the adornment, the seed, the complete `edge`
and the guarded `path$bf` component that architecture section 60 sketches.

| Component | State |
| --- | --- |
| `lib/magic.c` | finds what a query reads completely -- extensional and negated predicates and everything they depend on, as whole components of the program's schedule -- and rewrites the rest: an adornment worklist memoized on predicate and bound columns, body order from the planner seeded with the head's bound slots, adorned rules guarded by their magic predicate, one magic rule per demanded call, and before every demand call after the first a supplementary predicate `sup$<adorned>$<rule>$<call>` over the prefix's live variables, so generated bodies are linear in a body's calls; seeds from the query's own body; generated rules renumbered into a canonical form, deduplicated through a hash of it, and dropped when the head is one of their own positive literals; `max_magic_preds` and `max_magic_rules` enforced, naming the predicate being adorned; when neither is set, budgets of 256 plus 16 times the program's derived predicates, its rules, and its size in literals (each body plus one, a fact one), the last counting the body literals of every rule drafted, kept or dropped, a rewrite outgrowing one abandoned with a warning, and setting either limit lifts all three; the walk visits each component once and adornments read their rules from an index by head, so both are linear in the program; generated names interned with `$` and marked defined |
| `lib/plan.c` | the order is decided before any operation is emitted; `relic_plan_order()` returns that order alone from a given set of bound slots, and an incomplete relation is estimated without being created |
| `lib/stratify.c`, `lib/depgraph.c` | the analysis runs over any rule table, `relic_schedule_build()`, so the rewritten rules are stratified by the same code as the program |
| `lib/eval.c` | a query over a program not yet materialized, and not under `RELIC_F_NO_MAGIC`, is answered in two phases: the complete components evaluated from the program's schedule, then the generated rules from a schedule of their own, then the rewritten query; relations discarded first and left marked as the fixed point of no rules; a rewrite over its budget answered from the whole program instead; a semi-naive round runs only the variants whose delta literal reads a member that gained in the round before, in planning order, so it does exactly the work running every variant would at a cost proportional to what changed |
| `lib/explain.c` | the `magic rewrite of` heading with its `adorn` and `seed` lines, and the rewritten components nested one level beneath it |
| `include/relic.h` | `RELIC_F_NO_MAGIC`; the Magic limits documented as enforced when set and as budgets when not |
| `cmd/relic/main.c` | `-X no-magic` |
| `tests/` | `test_magic`: the rewrite against the whole fixed point over 24 generated graphs, ten programs -- left, right, double and triple recursion, same generation, mutual recursion, negation of a derived and of a recursive predicate, facts mixed with rules, constants in heads -- and five binding shapes, plus adornment names, memoization, duplicates, the dropped tautology, both limits at and below their values, a widening program of 2^10 adornments abandoned at its budget with the fixed point's answer while configured limits replace the budget, no state leaking between queries or into a later materialization, a load after a rewritten query reaching the next one, the literal budget abandoning a padded widening program and a program whose long rule calls its own predicate while facts behind a rule fit it, and generated rules and literals doubling, not quadrupling, when a body's calls double; the evaluation sweep in `test_exhaustion` runs through the rewrite as well, and a sweep over every allocation of a query whose rewrite outgrows its budget refuses them before and after it is abandoned; `tests/magic.sh` asserts that five bound queries over a hundred disjoint chains hold at most a tenth of the whole fixed point's tuples and answer the same; `tests/equiv/strategies.sh` compares every language test with and without the rewrite, and tuple totals within runs that evaluate the same rules; fourteen golden tests, among them the nested `-e` of both binding patterns of `path`, of a seed derived from another literal and of an adornment with no column, the materialized bypass, both limit diagnostics, the budget warnings on generated literals, and the supplementary rules of a body of three demand calls, with and without `-e`; `tests/perf.sh` answers a recursive cycle of 200 000 predicates through the rewrite within a tenth of its budget, which any of the three linear walks done by searching a component misses by minutes, and a body of 400 demand calls through its rewrite under a 16 MiB memory limit |

The six-edge chain of the S7 counters holds 13 tuples through the rewrite
against 27 from the whole fixed point. Over a hundred disjoint chains of fifty
nodes a query bound to one chain holds about 5,000 tuples against 250,000.

An unbound query reads its predicate through the all-free adornment, which
may call the predicate bound from inside and so derive about twice what the
whole fixed point does; `TODO.md` records subsumption as the remedy. The
stage is about 2,500 lines of library and tool code, comments included,
against a budget of 800; `lib/magic.c` accounts for 1,700 of them.

**The library interface, stage S11.** `include/relic.h` is the interface
`specs/design-api.md` specifies and `man/relic.3` documents: 29 entry points.

| Component | State |
| --- | --- |
| `lib/api.c`, `lib/diag.c` | a context is busy for the whole of an operation: every public entry point that changes it begins with `relic_enter()` and returns through `relic_leave()`, and entry points share workers rather than calling one another, so caller code run in any way -- a diagnostic, explain or row callback, or the read function of a stream being loaded -- finds its context busy and every call that would change it refused; a refusal is what `relic_error()` describes until the next error or until the operation returns, is not counted among the operation's errors, and calls no callback; reads -- `relic_string()`, `relic_error()`, `relic_value_compare()`, `relic_stats()`, `relic_query_count()` -- are allowed |
| `lib/rule.c` | destroying a query reads nothing of its context unless the query is being answered, so it may follow `relic_destroy()`; destroying the query being answered is refused |
| `lib/eval.c`, `lib/relation.c` | answers are transient: a query's answer relation is emptied once its rows have been delivered, whether the query succeeded or not, so answers never count against `max_tuples` or `max_memory` for a later query |
| `lib/value.c`, `lib/lex.c` | `relic_value_symbol()` and `relic_value_string()` intern caller bytes; a symbol must be spelled as the lexer spells one, `[a-z][A-Za-z0-9_]*`, and every symbol value the library accepts is checked against the same rule, so a symbol is an identifier wherever it came from |
| `lib/parse.c` | `relic_fact_add()` appends the fact as a statement of the program, exactly as its text would load, so it is checked, compiled, evaluated and invalidates what was derived by the same code as a loaded fact; the predicate name is held to the symbol rule and may not be `not`; `relic_load_file()` takes the source name before the stream, as the other loaders do |
| `lib/result.c` | `relic_query_collect()` and the result accessors, a client of `relic_query_run()`; a result holds no pointer into the context and outlives it |
| `cmd/relic/output.c` | `-s` collects through `relic_query_collect()` rather than holding rows of its own |
| `examples/embed.c` | facts added through the interface, rules loaded as text, one query streamed and stopped at its first row, one collected; `make installcheck` builds it with the flags `pkg-config` gives for the installed library and runs it |
| `man/relic.3` | every entry point, the structures an embedder fills in or reads, the callback types, the flags, the return convention and the callback rule; `mandoc -Tlint` clean |
| `tests/` | `test_api`, linked against the shared library through `<relic.h>` alone: every mutating entry point refused from a row, a diagnostic and an explain callback and from a stream's read function (`funopen(3)` or `fopencookie(3)`), a refusal in a diagnostic callback not hiding the error behind a failed query, a query destroyed after its context, answers not outliving their delivery under `max_tuples`, every diagnostic class reaching the callback with nothing on standard error, value constructors, eight refused facts and an added fact indistinguishable from its text, a fact added after materialization and after a direct query, a collected result equal to the stream and outliving its context; `test_exhaustion` sweeps a value built, added as a fact and collected, from an empty context so that the fact's statement makes the program arena's first allocation; `tests/symbols.sh` holds the shared library's defined dynamic symbols to the header's `RELIC_EXPORT` set in both directions; `tests/man.sh` holds the manual's `NAME` list to the same set and lints it; `tests/reentry.sh` holds every exported function either to a short list of those that never enter a context or to being tried from inside one by `test_api`, so a new entry point cannot go untested |

The step limit was deferred and `relic_config_t` is frozen without it. The
stage is about 390 non-comment lines against a budget of 250: about 430 in
the library, less about 40 the tool gave back by collecting through it.

**The interactive mode and the finished tool, stage S12.** `relic -i`
answers queries typed at a terminal or read from a pipe, and the option set
of architecture section 40 is complete, specified in `specs/design-cli.md`
sections 8 to 10 and documented in `man/relic.1`.

| Component | State |
| --- | --- |
| `cmd/relic/repl.c` | the session: each line blank, a `%` comment, a command or one query, prepared as `<query N>` and answered into the batch's output under the same `-F`, `-s`, `-n`, `-o` and separators; `.help`, `.plan QUERY` (the query run with its plan on standard output and its rows discarded, `-e`'s destination put back afterwards, diagnostics at the columns of the line as typed), `.quit`, `.stats`; replies on standard output, never the `-o` file; a failed line reported and the session continued, exit 1 if any failed; a NUL byte refused; everything answered flushed before each line is read; a failed read or write, of a row or a reply, ends the session |
| `cmd/relic/input.c` | the one place lines are read: libedit with emacs bindings, the user's editrc and an in-session history when standard input and standard error are both a terminal and the build has it, `getline(3)` otherwise; prompt `relic> ` on standard error under the same condition; any `getline(3)` failure but the end of input reported; a line without its newline, text at the end of input without one included |
| `cmd/relic/main.c` | `-i` as phase 9, standard input never read as a program under it, `-c -i` a usage error; `-n` attaches no output, so `-o` creates nothing; `-v` writes `query N: M rows` to standard error after each query, standard output flushed first; `-vv` sets `RELIC_F_NOTE_MIXED`; every query given to run numbered, session lines continuing after phase 8's |
| `cmd/relic/output.c` | an output with no stream counts and drops rows; `output_discard()` for `.plan`, `output_flush()` before each session line is read |
| `lib/eval.c`, `lib/plan.c` | `RELIC_F_NOTE_MIXED`: the first ordering comparison between values of different types in each rule reported as a note at the comparison, once for the life of the context for a program rule and once per answer for a query; every compiled rule records its origin, which Magic copies inherit, and the record of a note lives on the origin, so nothing is allocated; a FILTER records its comparison's location |
| `include/relic.h`, `man/relic.3`, `configure.ac` | the new switch; version-info `2:0:1`, an added interface |
| `man/relic.1` | options, limits, the interactive mode, the formats, exit status, examples |
| `tests/` | `tests/langtest.sh` feeds a test's `.stdin`, or else an empty input, so a session is a golden transcript: eight `repl-*` tests (a session, every command, every refusal and end of input part way through a query, `.plan` with `-e` and after `-a`, `-v` and `-vv` numbering, `-n`, json, `-c -i`) and four `verbose-*` tests; `tests/repl.sh` for a session with no operands, `-o` under `-i`, a NUL byte, a carriage return, blanks splitting a command, `.plan` and `.stats` beside `-o`, an escaped unknown command, `-n -i -o`, rows written before the first read, a failed read, and a failed reply and a failed write; `tests/man.sh` holds `relic.1`'s option list to the usage message and lints both pages; `test_api` notes each rule once across rewrites, later queries and materialization, two rules written at one location apart, a query once per answer together with the seed its rewrite copies the comparison into, and never without the switch; the equivalence suite skips sessions and leaves the `-vv` note out, since when it appears depends on the strategy |

The transcripts pass in a build with libedit and one configured
`--with-libedit=no`. The terminal path -- the prompt, libedit's editing and
history, end of input at a prompt -- was checked by hand through a
pseudo-terminal in both builds, and is not automated.

Build options: `--enable-debug`, `--enable-werror`, `--enable-asan`,
`--enable-lsan`, `--enable-msan`, `--enable-ubsan`, `--enable-coverage`,
`--with-libedit`. Test suites: `make check`, `make check-asan`,
`make check-lsan`, `make check-msan`, `make check-ubsan`.

`--with-libedit` decides whether the interactive mode edits lines; only the
tool links libedit, never the library.

**A query owns its answer, decision D-17.** A query's answer is a relation
the query object owns rather than the relation of a predicate, specified in
`specs/design-query.md`, so answering queries leaves the predicate and
relation tables as they were and a session costs each line what the program
holds rather than every line before it.

| Component | State |
| --- | --- |
| `lib/rule.c` | a query's rule has head `RELIC_ID_NONE` and every rule records its head arity; `relic_query_prepare()` and `relic_query_loaded()` create the query's answer relation, of the head arity and named by the query's serial, once the rule has compiled and before the query is published, and `relic_query_destroy()` frees it without reading the context; the program's queries record their serial beside their rule, since loads and preparations share one sequence; `relic_pred_is_answer()` is gone with the predicate |
| `lib/relation.c` | `relic_relation_create()`, `relic_relation_clear()` and `relic_relation_free()` serve the table's relations and a query's alike; `clear` is the one way a relation loses tuples and gives back what it counted, the discard of every relation is `clear` over the table, and an assertion confirms both totals are zero afterwards; a relation records its predicate or its query's serial, and `relic_relation_name()` and `relic_relation_describe()` name it as `-e` output and every relation diagnostic do, `$q<serial>` for an answer |
| `lib/plan.c`, `lib/eval.c` | the planner's EMIT writes the relation its caller names and confirms its arity is the rule's head arity; a component's rules derive into their members, a query's rule into the query's answer, emptied before the rule runs and after the rows are delivered |
| `lib/magic.c`, `lib/explain.c` | the rewrite reads every head arity from the rule or its draft and treats the query's head as a pseudo-adornment it never looks up; the explain output writes a query's head and every operation's relation through the relation's name, so the golden `-e` outputs are unchanged |
| `tests/` | `test_query`: 64 answers, through the rewrite and from the whole fixed point, leave the predicate count, the relation table and the tuple and storage totals as the first left them; names read from `-e` across loads and preparations; an answer refused by the tuple limit gives back the tuples and bytes it counted, and its query is destroyed after its context; `test_plan`: an EMIT writes the relation given and a relation of the wrong arity is an internal error naming it; `tests/perf.sh`: a session of 100 000 query lines within a tenth of the budget, about 3 s in a debug build, which the quadratic session missed by minutes |

**Planning costs what its plans hold, decision D-18.** A body is ordered
in passes, one per state of the bound set, and the memory an evaluation's
plans and a query's rewrite hold is charged to `max_memory`, specified in
`specs/design-planning-cost.md`. A fact is not planned: its turn in the
pass over its component inserts its head, so a program's facts cost their
tuples and nothing else.

| Component | State |
| --- | --- |
| `lib/plan.c` | a pass scores every positive literal not yet placed once, sorts them by bound arguments, estimate and body position, and places from the front of that ranking until a placement binds a new slot; tests are offered a place only when the bound set grows; the scoring and readiness functions take the bound set as an argument; the ranking is allocated for one call and freed when it returns |
| `lib/account.{h,c}`, `lib/arena.c` | the memory account: the relation store's storage and the chunks of the charged arenas, checked against `max_memory` before either grows; `relic_arena_init_charged()` attaches an arena, a chunk the limit refuses fails the arena as exhaustion does and is recorded on the account, and a reset gives the chunks back and keeps the arena charged; the evaluation arena and the rewrite arena are charged, the context, program, schedule, analysis and query arenas are not |
| `lib/relation.c` | the store's storage total is the account's `store_bytes`, and an insertion or index build is checked against the whole account |
| `lib/diag.c`, `lib/api.c` | `relic_diag_no_memory()` reports a recorded refusal as `limit exceeded: memory=N while <activity>`, once; `relic_diag_activity()` records the activity -- `planning <predicate>`, `planning <query>`, `rewriting <query>` -- and forgets an unreported refusal, as does the beginning of every operation |
| `lib/eval.c` | a component's base plans are its rules with bodies; one base pass walks its rules in program order, inserting a fact's constant head through the same insertion an `EMIT` makes, so counters and tuple order are unchanged; a fact's head is confirmed to be constants of the relation's arity once per evaluation; an evaluation names its first component, and a query's rewrite and plan name the query, before they allocate |
| `tests/` | `test_plan`: 60 generated programs of up to 30 literals, with repeated relations and variables, constants, wildcards, empty relations, comparisons and negations, ordered from nothing bound, from a generated bound set, and with generated relations assumed still derived, under the default planner and `-X no-reorder`, against a step-by-step reference written in the test; every golden `-e` output is unchanged; `test_arena`: a charged arena's account holds what it holds, through an oversize chunk and a reset, an uncharged one is not counted, and the limit refuses a chunk the store leaves no room for, stickily and recorded once, and the next chunk past it; `test_diag`: a refusal is reported as the limit with its activity, once, and forgotten by a new activity or operation; `test_query`: a recursive body's variants refused while planning its component, a long query refused while planning it, a rewrite refused while rewriting and not answered from the whole program, an admitted rewrite giving back its arenas, 20 000 facts evaluating within 16 KB of their relation, and a fact between rules inserting in its turn; `tests/cli.sh`: a one-byte limit refused while rewriting, a 3000-byte one while planning, and a limit admitting the plans and tuples refusing the index; `tests/perf.sh`: a recursive body of 8000 literals with 500 delta variants, and an 8000-literal body under 125 bound patterns, each within a fortieth of the budget and at least five seconds, about a second each where the step-by-step order took over 300 s and 153 s, and each refused under a 16 MiB limit with the limit's message within the same budget |

**Generated programs, fuzzing, coverage and benchmarks, stage S13.** Test
machinery that looks for the bugs the curated corpus cannot find, specified
in `specs/design-hardening.md`.

| Component | State |
| --- | --- |
| `tests/random/generate.c` | a program of up to eight predicates, 96 rules and four queries from one 64-bit seed through splitmix64; arity 0 to 4; extensional predicates at level 0, derived ones at 1 to 3, a positive literal reading at or below its head's level and a negated one strictly below, so every program is stratified; each derived predicate's first rule reading only lower levels; positive literals drawn first and every other variable from theirs, so every program is safe; symbols, integers including both extremes, and strings needing escapes; all six comparisons; atom and conjunctive queries |
| `tests/random/program.c` | the model's safety check, its printer, and the shrinker's four edits: drop a query, a rule, a body literal, or a predicate's column everywhere |
| `tests/random/compare.c`, `answer.c` | each program loaded and checked in one context per strategy -- default, naive, no-index, no-reorder, no-magic, the three combined with no-magic, and materialized; answers rendered to text, sorted and compared with the default's; tuple totals compared with the reference strategy's; a refused program, a failed query, differing answers and differing totals each a failure kind |
| `tests/random/main.c`, `shrink.c` | each program run in a forked child under an alarm, announcing each strategy and then its verdict on a pipe; a signal a crash or a hang, a nonzero status or a missing verdict abnormal, each charged to the strategy running and keeping its signal or status; a failure shrunk greedily while kind, strategy and signal hold, printed with its seed, and optionally written out; `-F` injects a fault and requires it found and shrunk to four statements or fewer |
| `tests/fuzz/` | `fuzz_load` loads and checks its input; `fuzz_eval` also answers every query under the default configuration and under naive without Magic, with tuple and memory limits, and aborts when both succeed with different answers; each linked with libFuzzer under `--enable-fuzzers` and always with a replay driver `make check` runs over `tests/lang` and `tests/fuzz/corpus`; a token dictionary |
| `tests/coverage-report.sh` | lines and functions per file of `lib/` and `cmd/relic/`, unioned over every object compiled from the file, with every function never entered and every range never executed |
| `bench/run.sh` | chain, random graph, grid, same-generation tree, negation-heavy task graph and a large fact file, generated with `awk`, each timed once with its `-X stats` counters; `bench/baseline.txt` one idle machine's run |
| `Makefile.am`, `m4/relic_sanitizers.m4` | `--enable-fuzzers`, refused where the compiler cannot link a libFuzzer target; `make check-random` over `RELIC_SEEDS` seeds from `RELIC_SEED_START`, a thousand from 1 by default; `make fuzz` for `FUZZ_SECONDS` a target; `make coverage` in a static instrumented tree beside the build |

`make check` runs the self-test, fifty seeds and the corpus replay. A
thousand seeds pass in about six seconds and ten thousand pass clean. Each
fuzz target ran for twenty minutes under AddressSanitizer,
UndefinedBehaviorSanitizer and LeakSanitizer -- 6.9 million inputs through
the load target and 775,000 through the eval target -- with no finding. Four
bugs planted in a copy of the engine were each found within a few dozen
seeds and shrunk to five statements or fewer. The coverage report reaches
96 percent of lines; `docs/todo-coverage.md` sorts the rest.

**Comparison with other engines.** One set of workloads run through relic,
SWI-Prolog, Souffle, clingo, Nemo, DuckDB and SQLite, every answer checked
against relic's before anything is timed, specified in
`specs/design-bench.md`.

| Component | State |
| --- | --- |
| `bench/datasets.sh` | the data-set generators, shared with `bench/run.sh`: one tab-separated file per relation, and the fact syntax relic, SWI-Prolog and clingo share |
| `bench/compare/run.sh` | set A, the eleven workloads of section 4.2, and set B, the fourteen of section 4.3 -- OpenRuleBench's transitive closure and same generation, as rbench writes them, on random graphs, the nonlinear closure, and Andersen and CSPA points-to analysis over generated programs -- in nine engine modes: relic, tabled SWI-Prolog, the Souffle interpreter, compiled Souffle with and without the magic transform, clingo, Nemo, and DuckDB and SQLite through recursive common table expressions, each single-threaded; the SQL engines `n/a` on the three workloads SQL cannot express; relic first, its answer the reference and refused when empty; every other engine verified under a time limit, then timed by `hyperfine`; median, fastest, standard deviation, user time, peak memory, rows, compile time and status per cell, and a summary relative to relic; a relic that is a script refused, here and in `bench/run.sh` |
| `bench/compare/<workload>/` | the hand-written programs of each workload, five for the Datalog engines and two for the SQL ones; `bench/compare/driver.pl` prints SWI-Prolog's answer relation |

Every engine agrees with relic on every workload it finishes within the time
limit. CSPA over the Graspan data sets, the recorded run and the scale sweep
are open in `TODO.md`.

### Verified

On macOS 15 with Apple clang, in-tree and out-of-tree:

* `./autogen.sh && ./configure && make && make check` succeed;
* `make distcheck` succeeds;
* the resulting tarball configures, builds, tests and installs with no
  autotools, autoconf macros or pkg-config on `PATH`;
* `--enable-werror`, `--disable-shared` and `--disable-static` all build and
  test clean, and `make install DESTDIR=...` installs and uninstalls cleanly;
* `make check-asan` and `make check-ubsan` pass; `make check-lsan` and
  `make check-msan` report that the platform does not support them and
  succeed;
* the installed shared library exports exactly the twenty-nine documented
  entry points and nothing else; the compatibility objects linked into it are
  not reachable;
* `make installcheck` builds and runs `examples/embed.c` against the installed
  library through `pkg-config`.

The relation store's suites were confirmed the same way, by injecting each
fault and rerunning:

* reporting a duplicate as newly inserted, which `tests/unit/test_relation.c`
  catches as a second insertion adding a tuple;
* comparing all but the last column of a tuple, which it catches as a
  five-thousand-tuple relation collapsing;
* a rehash that disagrees with the hash the insertion used, which it catches
  as every tuple becoming unfindable after a growth;
* counting the tuple limit per relation rather than across all of them, which
  it catches as the limit admitting more than it allows;
* checking the memory limit after the growth rather than before, which it
  catches as storage passing the allowance;
* an off-by-one in the end of a relation, and a range measured from the wrong
  first identifier, which it catches as a delta naming the wrong tuples;
* leaving the storage total unreconciled, and growing the column array four
  times as fast, which it catches as the total disagreeing with the relation
  and as the million-tuple envelope being left;
* storing a tuple's columns before the index has room for it, which
  `tests/unit/test_exhaustion.c` catches as a relation holding a duplicate
  after an allocation failure;
* storing relations inline in a growable table, which
  `tests/unit/test_relation.c` catches as the first relation moving, and
  losing its tuple, at the ninth predicate.

Two claims that no test reached until one was written for each are now
covered by `tests/unit/test_util.c`: that the capacity a growth reaches is
the capacity its predictor named, which is what makes the memory limit exact
rather than approximate, and that a sequence of words hashes to a stated
number, which is what would make a platform of the other byte order fail
where the difference is rather than somewhere far away.

The checker's own suites were confirmed to fail when the property they guard
is broken, rather than only to pass, by injecting each fault and rerunning:

* giving `_` a slot in a positive body literal, which
  `tests/unit/test_rule.c` catches as two wildcards taking two slots;
* never advancing the compile cursor, which it catches as a second check
  repeating the first check's diagnostics and doubling the rule count;
* numbering slots body-first rather than as written, which it catches as the
  wrong spellings in the first three slots;
* keying the predicate table on the name alone, which
  `tests/unit/test_pred.c` catches as `foo/1` and `foo/2` sharing an
  identifier;
* advancing the cursor past a statement a check ran out of memory on, which
  `tests/unit/test_exhaustion.c` catches as a statement that is never
  compiled at all.

Five of the tests are checks on the tree rather than on the engine, and each
was confirmed to fail when the property it guards is broken, rather than only
to pass:

* `tests/globals.sh` reports any C object in a writable section, and was
  checked against an injected `static` counter and an injected
  `_Thread_local` one, in both plain and instrumented builds;
* `tests/langtest.sh` was checked against an altered golden message and an
  altered expected exit status;
* `tests/langfiles.sh` was checked against an unregistered `.dl` file and
  against a file in `tests/lang/` belonging to no test;
* `tests/unit/test_exhaustion.c` compiles the library against an allocator
  that refuses the nth request and sweeps n across interning and across a
  load, asserting that the sweep reaches both a load that runs out of memory
  and one that completes, so neither branch can quietly stop being taken;
* `tests/ascii.sh` walks the whole tree and excludes what is generated.

No platform other than macOS has been exercised, because there is no
continuous integration yet. `TODO.md` records that as the highest open item,
and records the two checks whose behaviour on ELF is inferred rather than
observed.

The evaluation suites were confirmed to fail when the property they guard
is broken, by injecting each fault and rerunning:

* a fixed point that stops after one pass, which the recursion tests catch as
  missing paths;
* a scan that does not restart its range when it is entered afresh, which
  every join catches as missing answers;
* a comparison placed before its operands are bound, which
  `tests/unit/test_plan.c` and the comparison tests catch;
* an unordered pair satisfying `<=`, which `tests/unit/test_eval.c` catches as
  symbols passing an integer comparison;
* relations never extended after a later load, which it catches as a stale
  answer;
* a query's synthetic head not counted as an occurrence, which every query
  catches as a spurious singleton warning;
* a refused query consuming a serial number, which
  `tests/unit/test_query.c` catches as a misnamed answer;
* rows streamed before evaluation has succeeded, which
  `tests/unit/test_exhaustion.c` catches as rows from a failed query;
* a separator written before a query is known to succeed, and a JSON string
  whose literal is not escaped, which `tests/cli.sh` and the json golden test
  catch;
* a C1 control or a byte that is not UTF-8 written raw inside a literal, which
  the text and json golden tests catch;
* a predicate wider than 32 arguments admitted by interning, which
  `tests/unit/test_pred.c` catches;
* a negated literal left out of a plan, which the planner's completeness
  check turns into an error instead of wrong answers.

The analysis suites were confirmed the same way:

* scheduling components in completion order without sorting by stratum,
  negation not lifting a stratum, and components scheduled in reverse, which
  `tests/unit/test_stratify.c` catches as schedules out of order;
* reporting every negative edge of a component rather than the first,
  choosing a component's first edge in graph order rather than text order,
  and reporting components in completion order rather than text order, which
  the golden cycle tests catch as a repeated error or a misplaced one;
* rejecting nothing because negation is ignored, which every unstratified
  test catches;
* tearing the old schedule down before the new one is complete, and
  installing a schedule an analysis could not finish, which
  `tests/unit/test_exhaustion.c` catches as a lost schedule and a check that
  passes;
* never marking a component recursive, which the recursion tests catch as a
  fixed point stopping after one pass;
* a schedule left stale after a later load, and a rejection that is not
  sticky, which `tests/unit/test_stratify.c` catches as missing rules and a
  repeated diagnostic;
* a recursive Tarjan search, which the long-chain test catches by crashing on
  its one-megabyte stack, through the chain and the cycle each alone.

The semi-naive suites were confirmed the same way:

* delta variants that never run, which `tests/equiv/strategies.sh`, the
  closure tests and `tests/unit/test_eval.c` catch as missing answers;
* a delta scan that reads the whole relation, which is correct and slower,
  and which the pinned counters in `test_eval` and `stats-semi-naive` catch as
  the extra work;
* a variant for only the first recursive literal of each rule, which the
  `joined` case of `closure-double` and the equivalence suite catch; the
  transitive closures alone did not, because a path can be split either way;
* advancing only the first member's delta, which mutual recursion catches in
  `query-recursion`, `components`, `test_eval` and the equivalence suite;
* reporting exhaustion once per refused array rather than once per
  evaluation, which the wide sweep in `test_exhaustion` catches as two
  messages;
* the equivalence suite itself run against a missing source directory and
  against a tool that always fails, both of which it reports rather than
  passing.

The negation suites were confirmed the same way:

* an `ANTI_MEMBER` that is not inverted, which every negation golden test,
  `test_eval`, the evaluation sweep and the equivalence suite catch;
* a negated literal placed before its variables are bound, and one never
  placed at all, which `test_plan` catches, the second also as the planner's
  completeness error in every negation test;
* a later load that does not discard, which `test_eval` catches as a stale
  `ready(build)` and as a limit exceeded by the tuples an earlier evaluation
  held;
* a discard that keeps the tuple total, which `test_eval` catches as a limit
  exceeded after the discard;
* a row callback calling back into its context unrefused, which the
  re-entry scenario of `test_api` catches by crashing;
* a context left idle while a stream is read, and a load that does not enter
  its context, which the re-entry scenario catches as calls from a read
  function and from every callback accepted;
* a refusal that outlives the diagnostic callback that made it, and one
  that outlives the query whose row callback made it, which `test_api`
  catches as a failed query explained by the refusal and as a successful
  one leaving an error behind;
* a query destroy that reads its context, which AddressSanitizer catches as
  a use after free in `test_api`;
* a collector or an added fact that runs out of memory without saying so,
  which the interface sweep of `test_exhaustion` catches as a failed run
  with no error;
* an answer kept after its rows are delivered, which `test_api` catches as a
  third query refused by a limit two queries' answers used up.

The index and planner suites were confirmed the same way:

* a tuple not linked after the last of its chain, which `test_index` catches
  as walks that end early, and as a walk during insertion that visits one
  tuple of twelve;
* a discard that keeps the distinct-count cache, which `test_index` catches
  as a stale count after a refill to the same number of tuples;
* the planner without indexes, which `tests/perf.sh` catches as a run that
  does not finish in its budget;
* a memory projection that leaves out the secondary indexes, or counts them
  twice, which `test_index` catches as storage over the limit, or as a tuple
  that fits refused;
* a lookup that walks past the end its relation had when it started, which
  `test_eval` catches as a round too few;
* ordering by estimate alone, and an assumed size not divided per bound
  column, which `test_plan` catches as literals out of order.

The Magic suites were confirmed the same way:

* a negated literal read on demand rather than completely, which the
  equivalence scenario of `test_magic` catches in 148 comparisons;
* duplicate generated rules kept, and a rule deriving its own head kept,
  which the duplicate and tautology scenarios catch;
* adornments not memoized, which every recursive program of the equivalence
  scenario catches as the limit on generated predicates, set far above what
  any of them needs so that the failure is a failed query rather than a hung
  suite.

## Documents

| File | Contents |
| --- | --- |
| `specs/architecture.md` | target design, v0.2 |
| `specs/design-memory.md` | arenas, arrays, interning tables, the value table |
| `specs/design-parser.md` | lexical structure, the syntax tree, diagnostics and error recovery |
| `specs/design-program.md` | predicate identity, variable numbering, the compiled rule, safety |
| `specs/design-relations.md` | the tuple store, tuple identifiers, the full-tuple index, ranges, limits |
| `specs/design-plan.md` | the operations, the binding decisions a plan records, the textual planner, a query as a rule |
| `specs/design-stratify.md` | the dependency graph, components, strata, the unstratified diagnostic, the schedule |
| `specs/design-eval.md` | the executor, the naive and semi-naive fixed points, materialization, answering a query |
| `specs/design-index.md` | secondary indexes: chains in identifier order, building and extending, lifetime, accounting |
| `specs/design-planner.md` | when a component is planned, statistics, the estimate, greedy order, access paths, the references, explain |
| `specs/design-magic.md` | what a query reads completely, adornments and names, sideways information passing, generated rules, termination, limits, evaluation, explain |
| `specs/design-api.md` | the public interface: values, programmatic facts, queries and results, callbacks and re-entry, diagnostics, exported symbols, versioning |
| `specs/design-cli.md` | the phases of an invocation, which queries run, exit statuses, the output formats, `-n`, verbosity, the interactive mode |
| `specs/design-hardening.md` | the program generator, the differential driver, the shrinker, the fuzz targets, coverage and benchmarks |
| `specs/design-bench.md` | the comparison with other engines: engines and modes, workloads, data, answers, measurement |
| `specs/style.md` | code style, binding on all relic-owned code |
| `relic-plan.md` | fifteen stages, six milestones, decision record |
| `initial-spec.md` | the original Draft v1, kept for reference |

## Next

Stage S14, documentation, packaging and release: the complete manual pages,
a `README.md`, `NEWS`, `LICENSE`, the `contrib/` port skeletons and the
release tarball.

## Decisions taken

Recorded as D-1 through D-16 in `relic-plan.md` section 1. The three that
constrain everything else:

* **D-3** tuples are dense arrays of 32-bit interned value identifiers;
* **D-8** the plan representation and the executor exist from S5 and are not
  rewritten by later optimization stages;
* **D-9** the naive evaluator ships in the library as the differential oracle.
