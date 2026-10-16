# Counting a projected base relation

A full grouped-count query can run its producer into a private table of
keys and counts and publish only the answer. The producer is an ordinary
plan over one complete base relation. Its intermediate predicate keeps its
definition but acquires no replacement storage or summary binding. One
answering run owns the certificate, plan, collector and publication state.

Related: [query modes](architecture.md#query-modes),
[aggregates](design-aggregates.md), [planning](design-planning.md),
[executor](design-eval.md), [parallel evaluation](design-parallel.md),
[values](design-values.md#interning-computed-values),
[evidence](../bench/compare/recorded/duckdb-files-analysis/README.md),
[performance work](../docs/todo-benchmark-performance.md).

## Model and boundary

The following shape describes a grouped count over a filtered projection:

```prolog
kind(P, D, E) :- file(P), match(P, "/test_[0-9]+[.]") < 0,
    D = substr(P, 0, 7), E = substr(P, index(P, ".") + 1).
group(D, E) :- kind(_, D, E).
grouped(D, E, N) :- group(D, E), N = count { kind(P, D, E) }.
grouped(D, E, N)?
```

The counted query consumes `file` once, computes the same `kind` EMIT rows,
increments a count for each `(D,E)` and publishes one answer row per key.
It retains neither `kind` nor `group` nor `grouped`
as a derived relation. A later query of any of those predicates still runs
its original rules. `relic_materialize()` retains ordinary whole-program
evaluation.

The certificate recognizes one complete query shape, not a predicate whose
arbitrary consumers must be inventoried:

* The query consists of one positive atom over an answer predicate Q, with
  distinct, unbound variables exposing every column. It has no query-owned
  group rules.
* Q has one rule: a positive group guard G(K), followed logically by a count
  over P's complete tuples, with K as its fold key. Its head contains every
  key and the count once. G has one rule, exactly the projection of K from P.
* P has one rule over an extensional source S. Its prepared plan is one SCAN,
  followed by pure assignments/comparisons and EMIT. There is no second
  generator or nested fold. `relic_plan_derives_distinct()` proves that the
  full P tuples are distinct because EMIT retains every generated source
  binding. The fold counts those complete tuples, not a wildcard projection.
* P, G and Q have no base facts. Every computed column emitted by P belongs
  to K; its other columns hold already assigned domain values. Checked column
  maps relate P's head, G's key, Q's head and the query output.

Key and head permutations use those maps. Shapes outside this grammar take
the ordinary query path. This includes joins, recursive sources, multiple
producer rules, independent guards that create empty groups, additional
aggregates and partially bound roots. Their semantics require no cases in
the collector because they never reach it.

This strategy applies in query-directed execution with default resource
limits. An already materialized program, `RELIC_F_NO_MAGIC`, or an explicitly
configured tuple, memory or Magic limit keeps ordinary execution. The
certificate is revalidated at every run of a prepared query, after program
readiness and schedule confirmation. It retains no relation pointers across
loads. Existing per-predicate rule counts establish whether a definition is
unique. Locating those definitions and inspecting their literals share a
256-visit certificate budget, counting inspected rule headers as well as
literals. Exhaustion declines the shortcut; it adds no definition cache,
whole-program use inventory or cost model.

## Preparation and ownership

Admission occurs in the nonmaterialized branch of `compute_answer()`, before
the original Magic completion walk can materialize P. Normal derived-state
discard leaves the base S intact. A declined attempt returns to the original
root and ordinary query path; it never changes the program, predicate
definitions or the materialized-program flag.

The producer planner derives its output schema from P's rule. It takes no
physical destination merely to determine head arity. An ordinary running plan
checks that schema when paired with its insertion/delivery consumer; the
count collector checks P's arity and its key-column map when paired with P.
The final answer has Q's arity, which need not equal P's. No empty P relation
is created to stand in for a destination.

The per-answer owner holds the prepared producer plan, ordinary executor
scratch, private group tables, ordering workspace and unpublished final rows.
It calls `relic_exec_run()` and consumes its pending EMIT batches privately.
It does not call `execute_plan()` or `take_batch()`, which publish values,
counters and diagnostics as ordinary batches arrive. Expressions continue to
use the same postfix executor, built-ins and failure rules.

## The private collector

A group entry contains its typed key, a checked count and its earliest
logical producer occurrence. A hash is a filter; equality compares value
kind and complete payload, including embedded NUL bytes. Stored domain IDs
are decoded for comparison with computed pending values. Local pending
marker numbers are never semantic keys. Private key hashing uses the
context's seeded hash functions and exposes no seed, hash or probe count.

The collector retains only key columns. On a new key it reserves the entry,
index and any owned bytes before changing a count or installing an entry.
Existing source strings can be borrowed from the frozen context. Computed
text is copied before its executor batch is reused. The existing identifier
table and nonmoving arena provide storage; the collector has no relation
membership index over P's full emitted tuple.

The final collector has an 8 MiB capacity budget, including its index,
entries, owned strings and growth overlap. This is an
internal admission bound, not a user resource limit. All capacities are
charged to the ordinary temporary-memory account. Source rows and existing
executor scratch remain separately accounted. Exceeding the private budget
before publication declines the attempt and preserves ordinary execution.

## Parallel execution

Workers read disjoint, ascending ranges of S through the same prepared plan.
A logical occurrence is the range's position in source order and the EMIT
sequence within it. Each source row emits at most once, so this order is
exactly the serial producer order, independently of worker assignment.

The pool assigns ranges in increasing logical order, and a worker completes
one range before claiming another. Each worker therefore observes an ordered
subsequence of the serial producer stream. Its group entries, appended on
first occurrence and never reordered by index growth, are already ordered.
It keeps its partial table across executor batches and assigned ranges.
A 64-row EMIT return or work quantum does not drain or clear it. Partial
tables and the merge frontier share a second fixed 8 MiB budget, with the
table portion divided into private worker allowances. They introduce no
per-row synchronization or value interning on workers. Source tasks and the
final barrier use the existing
pool; the caller combines counts after workers stop.

The caller merges these ordered entry lists by first logical occurrence.
It uses the same key-retention routine, representation and capacity policy
as serial collection, copying computed bytes into the final collector rather
than borrowing partial-owned storage. Thus its first-key insertion and growth
sequence is the serial one. Counts add; the first entry for a key establishes
its position. There is no sort of all producer rows or all group entries.
Hash order and worker number determine neither group order nor value IDs.

A declined parallel attempt releases its partials and retries this same
counted query serially with the final collector's budget. Only a declined
serial attempt chooses the ordinary query path. This rule also covers every
unpublished final-preparation or reservation decline. Thus partial allowances
or worker count cannot choose a different successful logical strategy. There
are no periodic partial-combine waves, stored-source families or concurrent
group-table writes. Traversal, copying, growth, rehash and ordered merging
poll for interruption at bounded steps.

## Ordered publication

The order of group keys is their first occurrence in P's EMIT stream. Within
each key, pending values are visited in original P head-column order. All
producer key values are numbered before any count result. Count results are
then numbered in group order, and final tuples are laid out in Q's head order.
This reproduces P's value publication followed by G and Q: G introduces no
values, and P's omitted columns already have assigned values.

Publication uses the existing pending-value and prepared-append protocols:

1. Measure pending key and count values, obtain promised IDs, and form the
   complete final rows. Prove that every promise fits the identifier space.
2. Prepare the empty query answer's append and reserve both its storage and
   the pending values. Any declined preparation releases private append
   state and unreserves pending copies. A parallel attempt then retries
   counted execution serially; a serial attempt returns to ordinary execution.
3. Append the values and apply/finish the prepared answer append in one
   interval with no fallible operation, diagnostic callback or cancellation
   exit. No fallback follows the first visible append.
4. Release producer plans, scratch and private collectors, then use
   `deliver_gathered_answer()`. Do not run the ordinary query rule over the
   populated answer: that path clears it on entry.

Ordinary `relic_query_answer()` cleanup empties the answer on every exit.
No callback receives a row until the whole answer can be delivered. An
interrupt during delivery retains the existing prefix-delivery contract.

## Failure, diagnostics and counters

The collector keeps work counters private. An executor note or expression
error declines the private attempt; ordinary execution establishes the
diagnostic order and retained-value prefix. A note can accompany a successful
ordinary answer. An expression error still fails, because the same producer
expressions read the same base values.
An interrupt takes the ordinary interruption exit without starting an
expensive replay. Already reported preparation errors are final; replay is
never used to pretend a diagnostic callback did not happen.

Allocation or capacity refusal before publication can decline only when it
has reported nothing and changed no semantic contents. Retained reservation
capacity stays accounted. After publication, failure is the operation's
failure: no undo of context-owned values is assumed.

On success, counters describe executed work. Producer EMITs and final answer
rows count as derivations; only published answer rows count as tuple
insertions. Private hash probes and ordering visits are not relation scans.
A successfully shared producer pass counts as one parallel round. Discarded
attempts contribute no published counters. These counters and held/storage
totals differ from ordinary materialization, while equal counted executions
preserve them across worker counts except for the parallel-round counter.
Value IDs, answer order and decoded answers are preserved.

Explain names the counted producer, base source, group columns and final
answer, and records a decline when an attempted strategy is abandoned. A
trace of an attempted plan is not a published error or note. Interruption is
checked after a trace callback. Explain does not display materialization or
fold-index work as executed when it is skipped. The original predicates
remain available to a later query, with no
source substitution and no claim that the program's fixed point is current.

## Validation and complexity boundary

The runtime budget is 1,000 net additional C/header lines, including serial
collection, parallel partials and integration. One per-answer owner is the
boundary. A general reduction framework, alternate-source registry, virtual
index credit or second expression interpreter requires a different design.
The implementation is not enlarged to fit an inconvenient eligibility case.

Tests compare complete ordered answers and value IDs with ordinary execution,
and compare counted counters across worker counts. They exercise duplicate
input, many-to-one keys, key/head permutations, mixed types, NUL bytes, no
matches, reused prepared queries and later queries of P/G/Q. Near-miss rule
shapes must reach the ordinary evaluator. Explicit limits preserve ordinary
refusal behavior. Fault and interruption tests cover private collection,
ordering, each reservation, the publication boundary and delivery; no failure
may expose a half-published answer or use a freed source/scratch pointer.

Performance validation requires actual removal of intermediate tuples and
per-row worker retention, then measures query time, complete-command time and
RSS. The [forward plan](../docs/todo-benchmark-performance.md) sets adoption
gates. Expression interpretation and input interning remain real costs; the
design makes no claim that this query transformation closes the import gap.
