# Counting a projected base relation

A full grouped-count query runs its producer through ordinary evaluation
with a counted destination. Value publication, diagnostics and shared-round
failure handling belong to the evaluator; the destination consumes canonical
keys and counts. The producer is an ordinary plan over one complete base
relation. Its intermediate predicates keep their definitions and acquire no
replacement storage or summary bindings.

Related: [query modes](architecture.md#query-modes),
[aggregates](design-aggregates.md), [planning](design-planning.md),
[executor](design-eval.md), [parallel evaluation](design-parallel.md),
[values](design-values.md#interning-computed-values),
[source comparison](../bench/compare/recorded/duckdb-files-analysis/README.md),
[collector measurements](../bench/compare/recorded/counted-projection/README.md),
[shared-publication measurements](../bench/compare/recorded/counted-destination/README.md),
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
publishes their computed values through the ordinary batch boundary,
increments a count for each `(D,E)` and delivers one answer row per key.
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
discard leaves the base S intact. A declined certificate returns to the
original root and ordinary query path. Once counted execution starts, it
completes or reports its operation's failure; it does not restart the ordinary
query over a partly published value prefix. It never changes the program,
predicate definitions or materialized-program flag.

The producer planner derives its output schema from P's rule. It takes no
physical destination merely to determine head arity. A running plan checks
that schema when paired with its consumer. The counted destination checks P's
arity and its key-column map; the final answer has Q's arity, which need not
equal P's. No empty P relation stands in for a destination.

One answering run owns the certificate, producer plan and counted destination.
The destination's completed keys and counts are the final answer; they are
not copied into another relation. Its producer uses the evaluator's ordinary
executor and serial/deferred choice. There is no second expression interpreter, private
execution loop or second query-replay protocol.

The destination owns both checked projections: original P columns to stored
keys, and stored keys plus the count to Q's columns. Its result arity is the
key arity plus one, checked against Q during construction; the result map is
a permutation of those columns. Callers do not retain an independent output
map or configure another reader. The immutable worker view exposes the input
and key schema, with no writable count storage.

Executor scratch and pending room are sized for the original P EMIT arity,
not the narrower stored key arity. The destination checks both sides of its
projection when it is paired with the plan.

The original P plan remains intact through SCAN, assignments, comparisons
and EMIT. Projection occurs at the checked destination boundary, in original
P head-column order. Replanning a rule that emits only K is incorrect:
removing source identity columns can let planning replace a generating scan
with an existence test. The certificate proves distinctness of complete P
tuples before their emitted rows are projected to keys.

## One publication boundary and distinct consumers

The evaluator owns the sequence that takes an executor batch: account its
work, report its note, discard and report an execution error if present,
resolve and publish pending values, and pass canonical tuples to their
consumer. The ordinary interruption check follows that sequence. Tuple
storage and counting share it. Streaming delivery retains its existing proof
that its plan computes no pending values and cannot report an expression error.

The counted consumer receives assigned domain IDs. It owns an anonymous
canonical key relation and counts indexed by that relation's tuple IDs.
Neither is registered as P, G or Q. Their paired reserve/apply/cleanup
contract belongs to this destination: room for both keys and counts is
obtained before a new key is installed, and every installed key has its
initialized count. Existing relation storage supplies membership, insertion
order and memory accounting. Counting needs no raw-string equality, text
ownership or separate value numbering.

The destination grows with the number of distinct keys under ordinary
allocation and accounting rules. It has no private capacity cutoff that
abandons a completed prefix and evaluates P again. Allocation failure in
serial execution is a reported operation failure, as failure to store an
ordinary batch is. Explicitly configured resource limits select ordinary
query execution before this destination is constructed.

Set insertion and counting have a closed, explicit distinction at the
destination boundary. A set retains one occurrence of a key; a counted
destination adds contributions. Its schema, projection, reservation and
application operations express that distinction together. Consumers do not
each decide when values or diagnostics become visible. The parallel driver
does not recover the distinction from unrelated nullable fields or flags.

## Parallel execution

One complete producer pass uses the existing deferred-round protocol. Its
counted destination starts empty. Workers read the frozen source and context;
no value or destination is published until they finish and the merge has
reserved everything it needs. A private failure abandons that round and runs
the same producer with the same counted destination serially. It does not
reconsider the logical query strategy.

The ordinary worker pending-value table normalizes projected key values to
worker-local identifiers. Its candidate screen retains one candidate per
distinct key with a multiplicity, rather than one candidate per distinct P
tuple. A repeated key increases its first candidate's multiplicity. The
candidate stays at its first occurrence even when the repeat belongs to a
later task claimed by that worker. A 64-row EMIT return or work quantum does
not clear either table.

Existing pending-value compaction visits candidates in task, candidate and
column order and gives their values canonical promises. The existing merge
finds the first candidate for each key across workers. For a counted
destination it adds contributions into that first candidate and discards the
later contributions; ordinary set destinations still drop duplicates.
Each key belongs to one merge stripe, so this introduces no concurrent
accumulator update or per-row synchronization.

Each source tuple emits at most one P tuple, so the total multiplicity is
bounded by the complete source's tuple count. Counted construction establishes
this bound; ordinary set rounds do not accumulate multiplicities. The
representation must not enlarge ordinary candidate records merely to carry
counts that set destinations do not need.

The merge prepares key storage and counts together, then uses its existing
reserve-before-publish boundary for values and destination state. A declined
reservation leaves both unpublished and invokes the ordinary serial retry.
Once it applies, it cannot fail. The consumer owns the additional count
storage contract; screening, dense admission and append code do not acquire
independent counted-mode policies. Dense collection remains a set consumer,
selected through the same destination contract.

Counted construction admits exactly one complete producer round into an
empty destination. The set screen's rule that drops a tuple already held by
a target is therefore never used to discard a counted contribution. Support
for accumulating into an existing destination requires a different contract.

There are no counted-specific occurrence ranks, sorted partial lists, byte
allowances or parallel windows. Existing task order determines first key
order. Reusing whole-round deferral also avoids reconstructing the serial
executor's batch fences from independently started pieces: those fences
depend on read work and on which computed values are already published.

## Value order and final answer

Every potentially new P value belongs to its key. Omitting assigned non-key
columns and repeated keys therefore preserves the first occurrence of every
new value, provided projection preserves P head-column order. Serial batches
publish those values through the actual producer continuation. Deferred
execution resolves them in the same first-occurrence order before applying
its complete key/count result. All producer values precede count-result values.

After the producer finishes, the calling thread enumerates groups in insertion
order and publishes any unassigned count integers through the evaluator's
existing pending-value operation. Each batch covers at most 64 groups,
including groups whose count is already assigned. The publication operation
can commit descriptors without rewriting any tuples; this pass constructs no
second answer relation. Its work accounting, interruption points and failure
boundary are identical at every worker count.

Only after all count values are published does delivery begin. Raw counts
remain counts. A complete COUNT destination supplies canonical rows by reading
its keys and looking up the assigned count IDs through its checked result map.
A complete SET destination supplies the rows of its borrowed relation. One
caller-side delivery loop handles both through this existing closed
distinction. Reading a result row allocates and publishes nothing. This is
read access to a completed destination, not a virtual source, executor
operation or independently configurable reader.

Successful value IDs and answer order match ordinary P/G/Q evaluation. The
retained prefix on a resource failure while generating count results follows
these counted-result batches, not the read-quantum fences of the eliminated
Q fold. Different evaluation strategies can allocate different storage and
meet resource failures differently; counted serial and shared execution
retain the same failure contract. Explicit-limit execution stays ordinary.

The evaluator releases producer scratch before delivery. The answering run
keeps the key/count owner alive through the delivery loop and releases it on
every exit. The context's busy state prevents callbacks from changing that
owner or its values. No callback receives a row until the complete answer is
ready; interruption and early stop use the same delivery loop as an ordinary
gathered answer. The counted query leaves its ordinary answer relation empty
and does not rerun the query rule.

## Failure, diagnostics and counters

Serial execution uses the actual producer's batch boundaries. A note is
reported normally; an expression error discards its batch before interning
that batch's computed values. A failed shared producer round retries this
same serial producer from the unchanged context, using the existing deferred
protocol. Reported preparation errors are final. There is no counted-query
replay after a note callback or after value publication.

Failed evaluations deliver no answer rows and release their private
destinations. Already interned values retain their normal context lifetime.
Interrupts and note callbacks follow the ordinary serial/deferred contracts;
no worker calls user code. Failure paths neither rewind committed values nor
pretend a reported diagnostic did not occur.

On success, counters describe executed work. Producer EMITs and successfully
finalized result rows count as derivations. Only newly installed key tuples
count as insertions; no second answer store exists. Reading the producer
source and enumerating groups for count publication count as tuple scans;
membership probes, merge visits and final delivery do not. Finalized-row work
is counted after its value batch succeeds, while attempted group scans are
counted at the publication boundary. A successfully shared producer pass
counts as one parallel round.
Discarded shared attempts contribute no published counters. Private key tuples
and their storage are accounted while held and released when delivery ends.
The successful counted work and final held/storage totals match across worker
counts except for parallel rounds; they can differ from ordinary
materialization. Successful value IDs, answer order and decoded answers
are preserved.

Explain names the counted producer, its real operations, base source, group
columns and final answer. A trace is not a published error or note;
interruption is checked after a trace callback. Explain does not display
materialization or fold-index work as executed when it is skipped. Original
predicates remain available to later queries, without source substitution or
a claim that the program's fixed point is current.

## Validation and complexity boundary

The counted destination owns keys and counts; ordinary evaluation owns
publication and serial/deferred failure handling. Apply the
[complexity guidance](style.md#architectural-complexity) to that division across
the system. Reuse must remove the separate typed-key storage, value publication,
capacity policy and query-replay obligations, not conceal them behind another
interface. A general source-replacement framework or another expression
interpreter requires a separate design. Source size is a review signal, not
an acceptance limit.

Tests compare complete ordered answers and value IDs with ordinary execution,
and compare counted counters across worker counts. They exercise duplicate
input, many-to-one keys, key/head permutations, mixed types, NUL bytes, no
matches, reused prepared queries and later queries of P/G/Q. Near-miss rule
shapes must reach the ordinary evaluator. Explicit limits preserve ordinary
refusal behavior. Fault and interruption tests cover consumer reservation,
paired key/count application, shared-round retry, count-result batches and
delivery; no failure may expose a half-published answer or use a freed
source/scratch pointer. Identifier-ceiling tests compare counted serial and
shared failure prefixes, including batches mixing assigned and new counts.
All result values must be published even when the first callback stops
delivery. Constant keys must still count every P emission,
and repeated keys spanning several tasks must combine every contribution.

Performance validation proves that workers retain keys and their distinct
pending values, not one candidate per P row. It measures long computed keys
and high-cardinality keys as well as files, with ordinary joins and recursion
as controls for the common evaluator changes. The
[forward plan](../docs/todo-benchmark-performance.md) sets adoption gates.
Expression interpretation and input interning remain real costs; this design
makes no claim that query fusion alone closes the import gap.
