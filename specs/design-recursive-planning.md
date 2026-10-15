# Recursive plan selection

This document covers the boundary between planning and semi-naive
evaluation: a variant has at most two statically compiled join orders,
and each round selects one from the relations and views that round reads.
It defines candidate construction, the integer work estimate, index
preparation, selection and the equivalences that make the choice safe.

Related: [architecture](architecture.md#planning),
[planning](design-planning.md#delta-variants),
[evaluation](design-eval.md#the-semi-naive-fixed-point),
[storage](design-storage.md), [parallel evaluation](design-parallel.md),
[testing](design-testing.md),
[measurement rules](../docs/todo-performance-measurements.md).

## Model

A **variant** belongs to one recursive rule and designates one positive
body literal as its **delta literal**. Views are assigned from the original
body positions: that literal reads `delta`, earlier literals of the same
component read `old`, later ones read `current`, and complete predicates
read `all`. A candidate changes the nesting of those reads, never their
views.

A **plan family** contains one or two complete plans:

* **Delta-first** places the delta literal before the other positive
  literals, then uses the ordinary greedy planner.
* **Input-first** gives another literal priority over the delta when
  their bound-column ranks tie. Other literals keep their ordinary greedy
  ranking. An input can be complete or belong to the recursive component;
  this is not a test for a generated Magic predicate.

The family is constructed when its component starts. Each candidate has a
complete operation array ending in `EMIT` and names its index requirements
by relation and key mask. Retaining an alternative does not create its
extra indexes. A prepared running plan binds those requirements to existing
index positions; only prepared plans reach the executor and worker tasks.
Selection evaluates a read-only **work recipe** over the operations and
adds the acquisition cost of missing indexes. It executes no expression.

```text
compiled rule + distinguished delta + fixed literal views
    -> two structural ordering policies
    -> identical orders collapsed
    -> plans, index requirements and work recipes compiled once
    -> ordinary and delta-first plans prepared

round views + existing indexes
    -> integer execution and acquisition estimates
    -> one selected plan per due variant
    -> selected plans prepared; missing indexes built before execution
    -> ordinary round execution and ordered commit
```

The motivating shape is:

```prolog
r(X, Z) :- guard(X), edge(X, Y), r(Y, Z).
```

Scanning a small delta and looking up predecessors competes with scanning
the guard and its edges and looking up the delta. The guard remains a
candidate driver however its size compares with a guessed delta size.
The actual delta size affects selection only when the round begins.

## Invariants

* **Candidate availability is structural.** A guessed cardinality of the
  distinguished delta cannot remove the input-first alternative.
* **A literal's view is immutable within its variant.** Both plans read
  the same logical relations at the same round boundary.
* **Every retained plan is independently valid.** Binding analysis,
  earliest legal placement of tests, existence checks and `EMIT` follow
  the ordinary planner's contracts.
* **A published family is nonempty and complete.** Its count is one or
  two; every counted candidate has its operation array, access requirements
  and required recipe. Preparation binds access paths without replanning.
* **Only executable access paths enter the store.** An unselected
  alternative's requirement is plan metadata, not an empty or suspended
  secondary index. Every published index contains every stored tuple.
* **Estimation cannot change program state.** It is a pure function of
  const plans and the store at the round boundary. It neither updates a
  cache nor walks tuples, executes expressions, builds indexes or moves
  frontiers.
* **Selection is deterministic.** Its inputs are logical counts, fixed
  views, available access paths and plan data. Index availability follows
  deterministic preparation; time, worker availability, addresses, hash
  seeds and physical hash-table occupancy are not inputs.
* **Selection finishes before the round writes any tuple.** The selected
  plan remains the round item's plan during execution and serial fallback.

## Constructing the candidates

### Two ordering policies

Validate the distinguished literal once at the family boundary: its body
position exists, it is positive, and its predicate belongs to the
component. Both ordering walks receive that same validated identity and
the same view assignment.

The delta-first walk places ready constant tests and assignments, pins the
delta literal before the other positives, and continues the ordinary
greedy walk. Pinning does not move a test ahead of the bindings it needs.

The input-first walk scores an unplaced positive literal with this
lexicographic key:

```text
more bound columns first
non-delta before delta
smaller cardinality estimate first
earlier original body position first
```

The second field is considered only after bound-column rank ties. The
distinguished delta needs no guessed cardinality: it is the sole literal
with the delta field set, so no comparison between equally ranked
candidates reaches its estimate. Non-delta candidates retain the ordinary
complete-relation measurements and incomplete-relation estimates.

For the motivating rule, `guard` and `edge` initially outrank the unbound
delta regardless of its guessed size. If `guard` is the smaller input,
it binds `X`, `edge` is then preferred for its bound `X`, and the delta
is reached with `Y` bound. A delta whose constants give it a strictly
better bound-column rank can still go first. That is the explicit greedy
policy, not an effect of a cardinality constant.

The policies use the same bound-set passes and readiness closure as
ordinary planning. The extra ranking field adds no pass and no join-order
search. Ordinary rules, query plans and the order requested by Magic Sets
use the ordinary policy; constructing a variant does not change binding
propagation in the rewrite.

### Retention and emission

Decide both complete body orders before emitting either. Compare their
arrays of original literal positions, including tests and assignments.
Identical arrays collapse to the delta-first candidate: given the same
rule, views and configuration, replaying the same order produces the same
terms, slot kinds and operations.

Emit retained candidates in delta-first, then input-first order. Emission
records the exact index masks the operations require without building
indexes. Compile recipes from those operations, and identify each distinct
`(relation, key mask)` requirement once per candidate for acquisition
accounting. Requirements carry no cached statistics. The component keeps
its families private until emission and recipe construction succeed.

Prepare ordinary and delta-first plans through the existing relation/index
interface when the component starts. An index required by any such plan
is available to every alternative that uses the same relation and mask.
An index required only by alternatives is built when a selected plan first
needs it, as specified under [preparation](#preparing-selected-plans).

Keep the immutable candidate and the prepared running plan distinct at
the interface. A candidate names required masks; a prepared plan holds
valid index positions. The executor accepts only the latter, produced by
one preparation function after every required index is available. An
invalid index position is not an encoding for an unprepared plan.

Estimated costs do not prune candidates. Estimates are approximate, and
recursive cardinalities change. In particular, an estimate of zero or a
saturated estimate is not a proof that a plan can be omitted. This keeps
the retention rule independent of the statistics used for selection and
removes the threshold and `NEVER` selection states.

### Reference configurations

`-X no-reorder` produces one textual-order plan and takes precedence when
combined with `-X no-index`. With `-X no-index` alone, the family contains
one delta-first plan; repeated whole-delta scans are not an adaptive
candidate. `-X no-exists` changes liveness and operations through the
existing planner, and the recipes reflect the emitted operations.
Naive evaluation has no delta variants and uses ordinary plans.

A one-plan family is prepared at component start, bypasses estimation and
needs no recipe of its own. These are the existing reference modes, not
additional public configuration flags.

## Statistics at a round boundary

For each relational operation, **N** is the tuple count of the exact view
the executor resolves for that operation. Use the same view resolver as
execution; do not derive `old` or `current` a second way from rule text.
An aggregate reads its complete group relation.

For a lookup through an existing secondary index, **K** is its number of
distinct keys over the entire owning relation. It is the sum of the existing
per-stripe key counts. A const index accessor computes this sum without
probing the table, reading tuple columns or moving frontiers. For a
nonempty published index, `1 <= K <= relation tuple count`.

`K` is not the number of keys occurring in a delta or old view. The ratio
`N / K` is a uniform-distribution estimate over the available key space,
including keys absent from the selected view. Correlation, constants and
skew can make it inaccurate; accuracy controls cost, never correctness.
No per-round distinct-value scan, new histogram or per-tuple statistics
update is required.

### Direct reads and missing indexes

Read an existing index's count directly through the const accessor when
estimating its operation. Summing the fixed number of stripe counters is
bounded metadata work. There is no evaluation-owned key-count catalog,
sorting or binary search for statistics, tuple-count version, refresh,
or catalog-miss fallback. The estimator takes const inputs throughout.

A recipe follows the relation its operation actually reads, including a
base store when appropriate. Prepared access paths use stable index
positions; an unprepared requirement is resolved by relation and exact
mask against the indexes already present. Neither retains an address
inside a growable index array. A requirement is not a second index registry.

An absent index has no measured key count. For this case alone, use the
owning relation's tuple count **T** as the provisional `K`: the largest
possible key count and thus an optimistic fan-out estimate. An empty
relation has `T = 0`; the lookup formula already handles it. This is not
a measured count or a bound on actual query work under skew. It avoids
scanning data or building an index merely to decide whether to build it.
The acquisition charge below accompanies this estimate. Once built, an
index supplies its exact key count in subsequent rounds.

Using `K = 1` for every absent index would price a lookup as a whole-view
scan per binding and could suppress the useful alternative indefinitely.
The optimistic estimate preserves the opportunity to acquire an access
path, with its first-use risk tested explicitly on correlated and skewed
data. Candidate retention never depends on either estimate.

## Estimating work

The estimator walks a recipe with **B**, the estimated number of bindings
reaching the next operation, and **W**, accumulated work. Initially
`B = 1` and `W = 0`. A unit is one probe, tuple visit, simple operation or
emission. It is a ranking proxy, not a prediction in seconds.

For a lookup, the estimated total tuples visited across its incoming
bindings is:

```text
Q = ceil(B * N / max(1, K))
```

Rounding occurs after multiplying by the incoming bindings. Computing
`B * ceil(N / K)` would predict at least one result for every lookup into
a sparse delta, discarding the distinction between mostly missing keys
and populated keys. With `N = 0`, `Q = 0`.

| Operation | Work added | Bindings passed onward |
| --- | --- | --- |
| `SCAN` | `B * N` tuple visits | `B * N` |
| `LOOKUP` | `B` probes plus `Q` tuple visits | `Q` |
| `MEMBER`, `EXISTS` | `B` probes | Zero for an empty view; otherwise `B` |
| `ANTI_MEMBER` | `B` probes | `B` |
| `FILTER`, `ASSIGN` | `B` operations | `B` |
| `AGGREGATE` | `B` folds plus estimated group-row visits | `B` |
| `EMIT` | `B` emissions | End of recipe |

For an aggregate, group-row visits are `B * N` for a scan, `Q` for an
indexed lookup, and at most `B` for a full-key probe, zero when its relation
is empty. The recipe uses its emitted access shape. Aggregate domains,
overflow, expression values and comparison outcomes are evaluated only by
the executor. The estimator conservatively assumes a test or assignment
passes unless an empty positive view establishes otherwise.

Repeated-variable constraints in a scan and selectivity not represented by
an index are likewise left to execution. A recipe does not probe a
constant key merely to obtain a more precise estimate.

Work already accumulated remains when a later positive literal empties
the estimated bindings. A plan that reads many tuples before discovering
an empty relation therefore has positive cost. `B = 0` ends the arithmetic
walk; it does not skip or rewrite the executable plan.

### Index acquisition

Let **A** be the cost of preparing the candidate's missing indexes. For
each distinct missing `(relation, key mask)` requirement, charge:

```text
build_work = 2 * T
A = sum(build_work for each distinct missing index)
candidate_work = execution_work + A
```

`T` is the entire owning relation's tuple count, not the operation's view
size. The two units model reading each stored tuple and probing the new
index for its key. As with execution work, allocation, hashing width and
cache effects are abstracted; these are not measured CPU times. All
arithmetic saturates. An existing index has zero acquisition cost.

Charge each requirement once per candidate even if several operations
use it, including an indexed existence check whose execution recipe only
counts probes. Preparation makes every access path valid, so acquisition
is charged even if an estimated empty prefix makes a suffix unreachable.
Candidates are compared independently: two due variants that require the
same missing index each include its cost, although preparation builds it
once. This conservative duplication avoids a joint plan search.

An acquired index remains an ordinary, fully maintained secondary index.
Its future maintenance is real but depends on future derivations, which
this round's estimate does not predict. Controls include an alternative
used briefly before a long period of growth; first-use charging alone is
not evidence that this tradeoff is beneficial.

### Saturating integer arithmetic

All work and binding counts are `uint64_t` with saturating addition and
multiplication at `UINT64_MAX`. Saturation is a numeric result, not a
plan-state sentinel. Once accumulated work saturates, the estimator can
return that cost immediately.

The multiply/divide operation preserves a representable quotient even
when `B * N` would overflow. With `k = max(1, K)`:

```text
q = B / k
r = B % k
Q = saturating_add(saturating_multiply(q, N), ceil(r * N / k))
```

`N` and `k` are bounded by the tuple-identifier range, so `r * N` fits in
64 bits; operands are widened before multiplication. The final ceiling
is division plus a nonzero-remainder test, not addition of `k - 1` to the
numerator. If the tuple-count representation changes, this arithmetic's
bound changes with it and is enforced by a compile-time assertion.
No floating point, extended-width compiler integer or runtime dependency
is required.

### Worked example

Consider the motivating rule with 2,000 guard tuples, 100,000 edges,
2,000 distinct edge sources and 10,000 distinct edge destinations. The
recursive relation's existing first-column index has 10,000 keys. Let `D`
be its current delta size; assume the guard membership tests pass in the
estimate.

The delta-first plan scans `D` rows, looks up an estimated ten predecessors
per row, checks the guard and emits. Its estimated work is:

```text
D + (D + 10D) + 10D + 10D = 32D
```

The input-first plan scans the guard, looks up its edges, probes the delta
and emits. Its estimated work is:

```text
2000 + (2000 + 100000) + (100000 + 10D) + 10D
    = 204000 + 20D
```

A one-row delta selects delta-first, costing 32 against 204,020 units.
A million-row delta selects input-first, costing 20,204,000 against
32,000,000 units. Both candidates exist at both sizes. The comparison is
derived from the views and indexes; no fixed unknown-row estimate decides
whether the guard-driven order exists.

These expressions simplify to a threshold for this example. General
recipes contain changing view and key counts and integer rounding, so
the family stores recipes rather than assuming one threshold covers every
round. Estimating each complete plan also avoids assuming its suffix has
the same cost as the other candidate's differently ordered suffix.

For a chain of `E` edges and the distance rule with one filter and one
assignment, delta-first has estimated work `6D`. If the alternative's
recursive index is absent, input-first has work
`2E + 4 * ceil(E * D / T) + 2T`. Here `D <= E <= T`: the delta shrinks
while the whole relation grows. This comparison keeps delta-first and
leaves that index unbuilt. For `E = D = T = 4000`, the costs are 24,000
and 32,000. Tests cover the complete sequence of rounds, not only this
first comparison.

## Selecting and running a plan

The calling thread resolves the round's views and gathers its due variants
before executing any round item. For each family:

```text
if there is one candidate:
    select it
else:
    estimate delta-first execution and acquisition
    estimate input-first execution and acquisition
    select input-first only when its estimated work is strictly lower
```

Equal costs, including two saturated costs, select delta-first. Select
every due variant before preparing any of this round's newly chosen access
paths, so all comparisons observe the same index availability. Selection
allocates nothing and builds no index. Zero-length deltas continue to be
excluded by the due-variant scheduler; the estimator is nevertheless
defined for zero-length views.

The chosen running plan is stored in the ordinary round item. The serial
executor, workers and a declined parallel round's serial retry all use
that item. They do not select plans again after another item has inserted
tuples. This makes serial and shared rounds use the same statistics
boundary and the same plans.

Execution-work history remains per candidate for deciding whether a round
is worth sharing. Inserted-tuple history remains per variant, shared by
its candidates, because both derive the same set into the same target.
Timing and work history do not feed the cost selector.
The scheduler scales a plan's work history by its leading scan's range
when there is one. Task boundaries and worker count cannot influence
candidate choice or build an unselected access path. A selected plan
beginning with a lookup runs whole; parallelism does not override its
indexed access.

### Preparing selected plans

After selection and before any round item executes, the calling thread
prepares the chosen plans in due-variant order and access requirements in
operation order. The ordinary relation/index interface reuses an existing
mask or builds a full index over the relation's stored tuples in identifier
order. Another selected plan requiring that mask finds it already built.
Preparation performs no join-order walk and does not change views or terms.

A build publishes only a complete index, preserving the storage invariant
that every tuple is in every published secondary index. Requirements for
unselected alternatives never enter the relation's index list. There is
no inactive-index bit in insertion, merge, reservation or lookup paths.
After acquisition, ordinary insertions maintain the index until discard.

Before workers start, initialize a newly built component index's recorded
delta start from the member's current delta. Its chains include old tuples;
the ordinary frontier walk positions a lookup within the requested view.
The same preparation boundary that publishes executable access positions
establishes this condition. A selected plan is not reselected after its
index build reveals exact statistics; the next round uses those counts.

Only after all preparation succeeds are round items exposed for execution.
A prepared candidate can be reused in later rounds because its indexes
remain present and complete. Parallel refusal retries that same prepared
plan. The round never builds an index while workers or an executor can
hold a cursor into it.

### Internal boundaries

| Owner | Responsibility |
| --- | --- |
| `lib/plan.c`, `lib/plan.h` | Construct and validate orders, collapse identical orders, emit counted candidates, access requirements and immutable recipes; bind prepared plans |
| `lib/index.c`, `lib/frozen.h` | Expose the const sum of secondary-index key counts |
| `lib/cost.c` | Evaluate execution and acquisition work as a pure function of const inputs through the frozen-store interface; the module is compiled with `RELIC_FROZEN_ONLY` |
| `lib/eval.c` | Own candidates and prepared running plans, establish the round boundary, select all due variants, prepare their access paths and publish round items |
| `lib/explain.c` | Describe the family and deterministic selection rule using the planning document's text contract |
| `lib/round.c`, `lib/task.c` | Execute the selected operation array under existing round and view contracts |

The internal family is a counted array of at most two candidates rather
than separate nullable plan fields plus a threshold. A candidate's recipe
has at most one step per operation. Access requirements name relations and
masks; executable index references use stable positions. No public library
ABI or language syntax is added.

## Failure paths and limits

Orders, retained operations, recipes and access requirements live in
the charged evaluation arena. Normal index builds are charged through
the storage account. Candidate count is fixed, so a rule with `L` body
literals and `V` delta variants emits at most `2 * V * (L + 1)` operations
and a linear amount of recipe/access data.

Each ordering walk retains the bound-set pass limit of ordinary planning.
Requirement collection and coalescing are bounded by plan size; no
join-order search or data-sized planning structure is introduced.
Runtime estimation visits at most both operation arrays of a due variant.
Reading a key count visits the fixed number of stripe headers. Resolving
an unprepared mask inspects only existing index metadata. It never scans
all variants of an inactive component or tuples in a relation. Long-body
and many-round workloads bound the practical cost of this extra controller work.

Failure to allocate either retained candidate or its metadata fails
planning with the established memory or limit diagnostic. The family is
not published partially. An index build can fail during initial or
first-use preparation; its partial storage is released and no plan with
missing access paths executes. A preparation failure fails the evaluation
with the ordinary diagnostic, rather than silently selecting another
policy. Earlier derived state and any completed index builds follow the
ordinary failure/discard rules, and no answer is delivered.

Retention charges plan metadata; acquisition charges actual index storage
through the existing storage account. Indexes acquired for alternatives
can increase later storage and insertion cost, which performance acceptance
measures. Requirement counts and allocation byte sizes use checked
arithmetic; numeric cost saturation never substitutes for a storage-size
check.

Round selection is total and infallible over a valid family and published
store. Preparation is a separate fallible phase. All count arithmetic is
defined at its limits. Cost recipes are internal products of the same
validated plans the executor reads; their interpretation uses an exhaustive
operation classification rather than treating an unknown operation as free.

## Determinism and correctness

View assignment follows original body positions, so moving a delta
literal cannot turn an `old` read into a `current` read or repeat a
semi-naive derivation partition. Each candidate passes normal binding
analysis. Reordering joins and using the planner's existing existence
checks preserve the set of emitted tuples under the fixed views.
Existence checks can change derivation multiplicity, so equality of
candidate derivation counters is not a correctness requirement.

With a fixed selected plan, shared execution retains serial derivation
order, counters, identifiers and limit behavior. At the next boundary,
both executions therefore expose the same view sizes and key counts;
the selector chooses the same plan again. Preparation also follows the
same choices and order, so it builds the same indexes before serial and
shared execution. This induction establishes thread-count independence
without observing scheduling or elapsed time.
Repeated runs and hash seeds give identical plans, selections and output.
Different reference strategies need equal answer sets, not equal derivation
order or memory use.

The work estimate can be wrong on correlated or skewed data. That changes
performance and resource use, including whether an evaluation fits its
memory limit, but not the fixed views or derivation semantics. Successful
evaluations give the same answers. Performance validation therefore checks
both selection directions and existing strong workloads, alongside
correctness.

The units also abstract over expression cost, cache locality, hash probing,
frontier advancement and future index maintenance. The selector adds no
special weights for a workload or a thread count; measured regressions are
evidence to reassess the model rather than to add exceptions to candidate
eligibility.

Explain output is a static description of retained plans and the cost
comparison, defined only in [planning](design-planning.md#explain).
Per-round costs and choices do not add lines, so output length does not
depend on fixed-point iterations. Counts used internally expose neither
hash seeds nor addresses; formatting retains the printable-ASCII and fixed
line-buffer contracts.

## Validation

The independent references are textual-order semi-naive evaluation,
no-index execution, and naive whole-program evaluation on small inputs.
The regression suite first exercises the missing-candidate path on the
unfixed implementation. Validation follows the
[review and sanitizer gates](../docs/todo-performance-measurements.md#review-and-deliver).

| Concern | Required evidence |
| --- | --- |
| Candidate availability | Guards below, at and above the unknown-cardinality boundary retain a distinct input-first plan when the bound ranks permit it; predicate names and literal permutations do not confer eligibility |
| Independent reproducer | An explicitly guarded recursive program queried through whole-program evaluation, so bound-query factoring cannot hide a planning regression |
| Missing-candidate cost | For the guarded scale-4/8 reproducer, scanned-plus-looked-up tuples stay within twice the textual-order control, with identical answers; the counter bound fails on the unfixed planner |
| Both selection directions | The same family chooses delta-first for a small delta and input-first for a large delta; the worked example fixes the exact expected costs |
| Empty inputs | Work before a late empty relation is counted; a first empty scan costs zero; positive probes and negation have their specified different flow rules |
| Arithmetic | Zero and maximum counts, exact division, remainders, overflow before a representable quotient, saturated sums, and deterministic saturated ties |
| Direct statistics | Pure repeated reads leave the store untouched, reflect growth immediately, and use the whole index with each operation's own old/current/delta cardinality; derived and base relations stay distinct |
| Unused alternatives | Chain distances retains the input-first candidate but never materializes its extra index; stored index masks and charged storage match the delta-first control apart from plan metadata |
| First acquisition | Missing indexes use provisional counts and include whole-relation build work once per mask; all choices precede builds; the selected plan's result agrees with references |
| Acquired-index cost | Guarded closure retains its work-counter improvement; skew and brief-use-then-growth controls measure the risk of provisional statistics and future maintenance |
| Lifetimes | A later index build can relocate its array without invalidating a recipe or prepared plan; a newly built index starts at the member's current delta; discard and query reuse retain no access positions |
| Semantic coverage | Nonlinear and mutually recursive rules, repeated variables, wildcards, constants, assignments, aggregates and existence checks agree with references |
| Failure behavior | Planning and first-use index allocation failures publish no partial family or index and expose no unprepared round; memory limits charge both candidates and actual builds; parallel refusal retries the already prepared selected plan |
| Determinism | Repeated runs, hash seeds and thread counts agree on explain output and unsorted results; thread counts agree on execution counters other than parallel-round count |
| Complexity | Existing long-body, many-variant and many-round performance tests remain bounded; planning time, selection time and index memory are measured separately |

Extend planner/cost unit tests and explain goldens, then the evaluator,
equivalence and random-program suites. Performance counter bounds belong
in tests; wall-clock and peak-memory comparisons belong in the benchmark
artifacts. Profile the targeted M3 rows across scales and thread counts
and use nonlinear analyses and small workloads as controls. A repeatable
regression due to estimator overhead, skew or retained-index cost must be
resolved before enabling this policy by default.

## Alternatives not taken

* **A larger guessed delta cardinality.** It still makes candidate
  availability depend on an arbitrary number. A structural ordering field
  removes that dependency.
* **Always placing complete inputs first.** Small deltas often repay a
  delta scan; inputs can also belong to the recursive component. Two
  bounded structural alternatives express the choice without recognizing
  workload names or enumerating special rule shapes.
* **The product of final fan-outs as the whole cost.** It loses the work
  before a late empty input and does not describe differently ordered
  suffixes. The recipe accumulates each operation's estimated work.
* **One fixed prefix/delta threshold.** Other recursive views and their
  index key counts change, and rounding need not reduce to a single
  crossing. Comparing complete recipes avoids extra threshold cases.
* **Observed runtime or hash-probe feedback.** These vary with scheduling,
  cache state and random hash seeds and would make choices nondeterministic.
* **Per-key sampling or per-round distinct counts.** Both add data work to
  selection. Maintained index counts provide a bounded initial estimator;
  its limitations are measured on skewed controls.
* **Replanning or enumerating join permutations during rounds.** Candidate
  count and construction cost would depend on runtime state. Immutable
  plans and a bounded selector keep the executor and failure boundary small.
* **Additional storage counters maintained by every insertion path.**
  Summing existing stripe counts needs no additional counter or list of
  writers that must participate.
* **A key-count statistics catalog.** Caching a small bounded sum adds
  construction, lookup, invalidation reasoning and mutable estimator state.
  Direct const reads remove that machinery; a cache needs measured evidence
  that counting itself is a bottleneck.
* **Eager indexes for every alternative.** An unused plan then adds memory
  and maintenance to every insertion. Retaining its access requirement
  separately removes that cost by construction.
* **Suspending a published index.** This would weaken stored-implies-indexed
  and introduce coverage and catch-up states throughout the storage paths.
* **A separate delta index each round.** It introduces a new lifetime and
  repeated builds. First-use full indexes reuse the existing chains,
  frontiers, append and failure contracts; repeated-build tradeoffs require
  separate evidence before adding another index representation.
