# Ordered driving cursors

This document defines how parallel evaluation divides a plan's first
relational generator into tasks, whether the generator scans a relation
or follows a secondary-index chain. It sits between selection of a
prepared plan and execution of a deferred round. The split preserves the
plan's access path, views and logical derivation order, uses bounded task
storage, and charges its preparation work to the scheduling decision.

Related: [architecture](architecture.md#parallel-evaluation),
[planning](design-planning.md#access-paths),
[recursive plan selection](design-recursive-planning.md),
[evaluation](design-eval.md#the-executor),
[parallel evaluation](design-parallel.md),
[storage](design-storage.md#secondary-indexes),
[memory](design-memory.md), [testing](design-testing.md).

## Model

A **driver** is the first `SCAN` or `LOOKUP` in an eligible plan. Before
it are only tests that bind no slots. Such a **prefix** either rejects
the plan or reaches the driver once, with no bindings to transfer to
another executor. Every key column of an indexed driver is consequently
a constant. An assignment or aggregate before the first generator makes
the plan ineligible for slicing; it remains an ordinary whole-plan task.
There is no search for a different, deeper generator.

The driver's **input sequence** contains the tuple identifiers it visits
inside its resolved view, before residual term matching:

* For a scan, every identifier in the half-open view `[first, end)`.
* For a lookup, the chosen key's chain identifiers in that view, in
  increasing identifier order.

The cardinality `M` is the length of this sequence. It is neither the
whole relation's cardinality for a lookup nor the number of tuples that
survive repeated-variable checks, filters or the rest of the join.

A **slice** is a contiguous subsequence of the driver's input sequence.
Its representation is a starting tuple identifier and an exclusive
identifier fence. A scan advances by one; a lookup follows `next`.
Neither uses a materialized list of matches. The plan supplies the
relation, access kind and index position; the task supplies only its
slice. The slice is subordinate to the operation's original view.

```text
selected and prepared plan + fixed round views
    -> execute a binding-free prefix once, privately
    -> resolve and count the driving sequence
    -> estimate suffix work and slicing cost
    -> choose ordered slices and locate their starts
    -> run the ordinary executor within each slice
    -> merge candidates in item, slice and derivation order
```

The caller prepares every task before publishing the derivation job.
The store stays frozen from preparation through the workers' completion
barrier. A refused attempt releases private state and reruns the same
selected plan serially. Slicing neither chooses a different candidate nor
builds an index.

### Workload shape

For a rule such as:

```datalog
pair(A, B) :- row(A, V, 7), row(B, V, 13).
```

the first lookup enumerates rows keyed by column 2, and the second uses
columns 1 and 2. The driver can be much smaller than the source relation
and each driver tuple can produce many answers. Slicing retains both
lookups. The useful parallel work is the entire suffix under each driver
tuple, including its downstream lookups, computations and emissions.

## Invariants

* **The selected plan is independent of the worker count.** Slicing
  changes only where that plan's execution runs. Plan selection completes
  before acquisition of its indexes, and acquisition completes before
  any cursor is opened.
* **Slices partition visits, not accepted bindings.** Their ordered
  concatenation is the original driver sequence, with each identifier
  once. Ordinary term matching remains in the executor.
* **Each operation retains its assigned view.** A driver slice narrows
  its cursor, never a relation's delta or another operation's view.
* **The prefix has one logical execution.** It leaves no binding or
  pending value for a task to inherit. Its counters and note are private
  until a successful merge and are committed once in item order.
* **A slice cannot backtrack above its driver.** The executor's lower
  level boundary enforces this; tasks do not replay the prefix.
* **A slice is usable only within its frozen round.** It carries no
  pointer into columns, chain links, key entries or a growable index array.
* **Preparation never changes semantic state.** It publishes no values,
  tuples, diagnostics or public counters. Its only storage-side write is
  ordinary frontier positioning at the operation's actual delta start.
* **Task metadata is bounded by the program and worker count.** No
  temporary or persistent array has one entry per driving match.

## Finding the driver

The planner records a driver position, or absence of one, in the prepared
plan. It determines this while walking the completed operation order,
using the operation semantics in `lib/plan.h`. The prefix consists of
`MEMBER`, `EXISTS`, `ANTI_MEMBER` and `FILTER`: operations that bind
nothing and succeed at most once. The first generator is the driver.
An `ASSIGN`, `AGGREGATE` or `EMIT` reached first terminates recognition.
This is a structural certificate, not an estimate of what a particular
input happens to make single-valued.

Binding analysis proves that every prefix operand is a constant and that
the driver's key is constant. A prefix filter can evaluate a constant
expression, including a string function; it still produces no persistent
binding. Its temporary allocations and possible errors use the ordinary
executor's rules. Recognition does not evaluate expressions or suppress
their diagnostics.

The certificate is derived separately for each retained plan. It does
not alter greedy ordering, delta-variant construction, access masks,
existence-check selection or explain's operation order. Static Magic
planning needs no cursor or scheduling metadata. A plan without a driver,
a seed, and an ineligible plan can still run as whole tasks beside other
items of a shared round.

## Preparing an item

### Execute the prefix once

The executor offers a preparation entry that runs the certified prefix
and stops immediately before its driver. It uses the same step functions
and quantum mechanism as whole-plan execution. It returns progress,
prefix rejection, driver reached, or a recorded failure. It never enters
the driver, executes `EMIT` or produces a candidate batch.

Worker 0's scratch runs prefixes before the derivation job starts. A
prepared-item record takes the prefix's read counters and first mixed-type
note, then the scratch's corresponding records are cleared. This gives
each count one owner: the item owns prefix counts, and workers own suffix
counts. The item also records whether the prefix rejected. A prefix error
declines the entire round; it is never reported out of item order.

Function scratch is opened before prefix execution, through the same
round-scoped text-buffer ownership as task execution. Reusing worker 0's
scratch preserves its capacity peaks through the job and the merge's
serial-memory-floor calculation. Prefix-only growth is included even
when the prefix rejects and the worker executes no suffix. Releasing or
resetting logical text contents must not erase those peaks. No frame,
text pointer, regex workspace or pending marker is copied into a task.

A false prefix creates one empty task record for the item and performs
no driver lookup. Its measured counts and note still belong to the item.
This keeps item order, insertion histories and diagnostics defined even
when the item produces nothing. Other items can make the round worth
sharing. If they do not, the private preparation is discarded and serial
execution establishes the ordinary result.

### Resolve the view and open the cursor

All member views come from the same round environment used for execution.
The eligible-round check precedes prefix evaluation: an `ALL` read of a
round target cannot become safe merely by being in a prefix. For eligible
rounds, an `ALL` end is stable through preparation and all tasks.

For a scan, the start and fence are the resolved view and `M` is their
difference. For a lookup, the ordinary key packing and exact key comparison
find the chain. `ALL`, `OLD` and `CURRENT` start at its head and stop before
the view end. `DELTA` first positions at the real delta start, using the
ordinary frontier invariant, and then applies the same end fence. Missing
keys and a first identifier at or beyond the fence give an empty sequence.

Cursor opening and walking live behind `lib/frozen.h`. The index owns key
packing, sentinel handling and frontier access. The round code never
reads key-entry or `next` arrays directly. The helper returns identifiers,
counts and positioning progress into caller-owned storage, not writable
index storage. Index positions are resolved against the relation at use.

Preparation's delta positioning is resumable in bounded groups of links.
Its private seek state records the key reference and current identifier
within the frozen index. It publishes the ordinary frontier only after
reaching the stop for the real delta start; cancellation leaves either
the original frontier or that valid stop. An exhausted seek retains the
chain's last identifier for the frontier, so later appends remain reachable.
The existing non-resumable lookup entry can drain the same storage helper;
there is one definition of the frontier invariant.

The seek helper reports links followed as preparation work. Counting and
locating boundaries also return after a bounded number of links. The
caller checks the interrupt flag between these returns. Constant-prefix
probes retain the executor's documented bound of one unresumable step;
slicing adds no unbounded, uninterruptible chain traversal.

## Constructing slices

### Two bounded passes over a lookup chain

The first pass counts the `M` chain identifiers inside the view, starting
at its resolved first match. It reads chain links, never relation tuples,
and retains just the first identifier, view fence and count. It does not
run residual matching or the suffix. This exact count is what scheduling
uses for the driver: an index's distinct-key count is not a particular
key's length, and `relation tuples / keys` is not a measurement of it.

After the caller chooses `P` nonempty pieces, their boundaries in match
ordinals are:

```text
b[j] = floor(M * j / P),       0 <= j <= P
```

For `M > 0`, `1 <= P <= M`. Adjacent pieces differ by at most one visit.
A second forward pass locates the identifiers at ordinals `b[0]` through
`b[P-1]` and writes them directly into the reserved task records. Each
piece's exclusive fence is the next piece's starting identifier; the
last uses the original view end. For a scan these identifiers are obtained
arithmetically without either traversal.

For example, a view whose lookup sequence is `[3, 9, 12, 20, 28]`, ending
at 31, splits into three slices:

| Match ordinals | Start | Exclusive fence | Identifiers visited |
| --- | ---: | ---: | --- |
| `[0, 1)` | 3 | 9 | 3 |
| `[1, 3)` | 9 | 20 | 9, 12 |
| `[3, 5)` | 20 | 31 | 20, 28 |

The gaps between tuple identifiers do not matter. A lookup stops before
the next piece's start, and chain order proves it cannot skip a member of
its own piece. It stops at `RELIC_ID_NONE` as well; zero is never ordered
as though it were a valid next tuple. An empty sequence has one empty task
and no usable starting identifier. A single-piece lookup uses its already
resolved start and needs no boundary pass.

For each indexed item, setup follows at most `F + 2*M` links, where `F`
is positioning toward the real delta start. The second pass can stop at
the last piece's start. There are a constant number of key probes, not
one probe or head-to-slice walk per worker. The sequence is subsequently
visited once by the workers, collectively, as the unsliced executor
visits it. Preparation links do not count as evaluated tuples.

### Bounds and storage ownership

`P` is clamped by `M` and `RELIC_CHUNKS_PER_WORKER * W`, with `W` the
actual pool size. The task array therefore holds at most
`items * max(1, RELIC_CHUNKS_PER_WORKER * W)` entries. Prepared-item
records are one per item and seek scratch is constant-sized. Both belong
to the pool's program-and-worker-sized control storage; their capacities
may persist between rounds, but all cursor contents expire at round end.
They have the same memory-account exemption as the existing task array.

Every capacity sum and byte multiplication is checked before growth. The
products in ordinal arithmetic use a sufficiently wide unsigned type:
tuple counts are below `2^32`, but arithmetic is widened before multiplying
by a piece ordinal. The implementation checks or proves the piece-count
bound rather than relying on the host's `size_t` width. Allocation refusal
declines the round without a diagnostic.

There is no per-key count, skip array or persistent match directory added
to an index, and no insert-time maintenance for a plan that never slices.
Candidate buffers, pending values and merge staging remain data-sized
charged storage under the parallel design; the bound on slice metadata is
not a claim that the whole deferred round uses `O(W)` memory.

## Scheduling by useful work

### Keep selection separate from scheduling

The recursive candidate estimator remains a pure function of const
recipes, views and existing index key counts. It neither opens a cursor
nor uses the pool. The scheduler measures a selected, prepared driver's
sequence only after that choice. A worker count can change task boundaries
and whether execution is deferred; it cannot change the chosen body order,
index acquisition or the semantic work counters for that plan.

At one configured or available worker, the ordinary path performs no
prefix preparation, chain counting or boundary walk. `RELIC_F_PARALLEL_ALWAYS`
is the deliberate exception: it exercises the deferred machinery and
finest allowed slicing at one worker as well as many. A pool reduced by
thread-creation failure or `fork()` follows its actual worker count.

A cheap preliminary check uses plan structure, resolved view sizes and
the suffix cost model before starting a pool or walking a chain. For a
lookup driver it substitutes the whole view size as an optimistic driver
count, rather than assuming one match or using the average key length.
It can decline a predicted small round; it is only a performance heuristic,
since downstream averages can still miss skew. It does not use that
optimistic count to make slices. Forced deferral bypasses this check.

### First execution and measured history

For a prepared item whose prefix succeeds, estimate suffix work starting
with `M` driver visits and `M` bindings entering the next operation. This
includes rejected driver tuples in the estimate; their residual selectivity
is unknown. Walk the remaining operations with the execution cost rules
from `lib/cost.c`: downstream scans multiply bindings by their view size;
lookups use the view size and the existing index's exact distinct-key
count; tests, computation, folds and emission add their operation cost.
Zero bindings stop the walk. All arithmetic saturates rather than wraps.

This is the execution part of the existing model, with a measured driver
override. It excludes acquisition cost because the selected plan's
indexes are already prepared. Share the operation classification and
arithmetic with recursive estimation; do not introduce another statistics
catalog, key comparator or independently maintained set of costing rules.
Ordinary plans need this walk too, even when they have no candidate recipe.
A direct walk of their immutable operations uses the same costing helper
without allocating a copied recipe solely for scheduling. An ineligible
item uses its whole-plan execution estimate and one indivisible task;
a seed contributes one emission. Neither invents a driver cardinality.

History belongs to the selected physical plan, not to the rule or another
candidate. A successful execution records its actual driver visits and
the work in the driver and suffix separately from prefix work. For a
positive previous count `M_previous`, estimate:

```text
suffix_work = saturated_ceil(previous_suffix_work * M / M_previous)
```

With no usable history, an unreached driver or a previous count of zero,
use the structural suffix estimate. The current prefix is measured once
and never multiplied by `M / M_previous`. Inserted-tuple history remains
shared by equivalent variants as the candidate-reservation design states.

Serial execution obtains driver visits from the executor's own traversal,
without a preparatory count. The dispatcher can accumulate the difference
in read counters around a driver step, outside the per-tuple loop. For a
slice the same observation covers just its piece. Only completed items of
successful rounds update history; speculative preparation and abandoned
tasks supply no sample. Driver metadata and this observation have constant
space cost and no public counter.

### Pieces and the preparation charge

Desired pieces follow estimated suffix work divided by `RELIC_CHUNK_WORK`,
clamped to at least one and to the match and worker bounds above. Prefix
work does not create pieces. `-X parallel` requests the finest allowed
partition. A single driver tuple is indivisible even when its suffix is
expensive; a deeper join split is a different scheduling design.

The sharing decision includes serial preparation. It accounts for the
prefix's executed operations and reads, the first pass and any delta
positioning, the predicted second pass, task setup and the pool's startup
threshold. These are costs, not public evaluation-counter increments.
The conservative policy requires total estimated suffix work to reach
`RELIC_PARALLEL_MIN_WORK` and exceed four times the round's total
chain-preparation link work. Each lookup contributes its positioning and
count pass, plus a boundary pass only when it has multiple pieces. Scan
drivers incur no link-preparation charge. Constants are shared policy
parameters measured with the benchmarks, never workload-name tests.

It also requires useful independent tasks. For per-item suffix estimates
`E[i]` and proposed piece counts `P[i]`, a simple optimistic critical-path
estimate is the larger of `ceil(sum(E) / active_workers)` and
`max(ceil(E[i] / P[i]))`, where active workers are bounded by nonempty task
count and pool size. Sharing requires this plus preparation and the
startup allowance to be smaller than `sum(E)`; ties decline. Saturated or
otherwise inconclusive comparisons decline, unless deferral is forced.
This prevents a large indivisible item from justifying a barrier by itself.

The cost of the count pass is real even when the final decision declines;
measure it on rejected rounds as well as accepted ones. Merge cost and
skew within a piece can make these estimates optimistic. Neither uniform
key averages nor equal driver counts prove equal suffix work. Several
ordered pieces per worker mitigate ordinary skew; one driver tuple with
almost all downstream work remains a known granularity limit.

## Executing a slice

The slice start entry resolves the whole plan's views as ordinary startup
does, checks the certificate and slice bounds, and sets the executor's
lowest runnable level to the driver position. Whole-plan execution uses
zero as its lower level. Prefix preparation has its own stop boundary;
there is still one operation dispatcher and one implementation of term
matching, arithmetic, probes and folds.

The driver's cursor is initialized directly from the prepared start and
fence. It is not initialized by narrowing a delta view and asking
`relic_index_first_since()` to find the slice. Only the real delta start
may advance a shared frontier: different slice starts are not deltas and
must never be passed to that API. This avoids both a frontier invariant
violation and `P` repeated walks to find starts already prepared.

The driver begins in cursor-resume mode. Resuming a scan or lookup reads
the next identifier from that cursor, increments the ordinary tuple-read
counter and runs normal term matching. A lookup still checks repeated
variables and other residual terms; the preparer has tested only its key.
The first successful match binds the frame exactly as in whole execution.
Every operation below it starts and backtracks normally within its own
original view, including another use of the same relation and index.

The driver never restarts within a task. Once it exhausts its slice,
backtracking at the lower level ends the task. Quantum returns, rejection
of many consecutive tuples, batch-full yields immediately after `EMIT`
and yields before a pending-value batch fills retain the same cursor and
lower boundary. No yield path silently restores level zero.

Empty tasks enter no executor. An eligible single-piece item still starts
at the driver after its once-executed prefix. Ineligible items run through
ordinary startup from level zero and have no prepared-prefix contribution.
Thus a caller cannot accidentally count a prefix both in preparation and
inside a whole-plan task.

## Counters, notes and commit order

Task enumeration is `(item ordinal, slice ordinal)`, independent of which
worker finishes first. Each slice uses the ordinary serial join order
within its boundaries. [Bounded routing](design-round-routing.md) can
pause a lane repeatedly and collect tuples out of order; the task ordinal
and within-task emission ordinal preserve the logical sequence. Tuple
owners retain minimum positions, and finalization orders those positions
before first-occurrence numbering of tuples and pending values.

The successful round's public read counters are the sum of each item's
prefix contribution once and every task's suffix contribution. Derivations
come only from task emissions. Prefix reads are not attached to every
slice; preparation link steps, key probes for slicing and boundary counts
are absent from public execution statistics, just as ordinary frontier
positioning is absent from tuple-lookup counts. Separate internal counters
make preparation costs visible to tests and profiling.

Notes are reported in item order, with the prefix note before that item's
task notes, using the existing rule-level note suppression. A prefix note
can be the first note even for an empty driver or rejected prefix. Prefix
or task failures report nothing from the speculative attempt: serial
fallback reestablishes both the earliest diagnostic and limit behavior.

Successful history recording includes prefix plus task work in the item's
total while retaining the separate driver/suffix quantities used for
prediction. Empty items still record zero insertions and an unreached or
zero-length driver as appropriate. Neither the task array nor internal
preparation counters are exposed as language or API behavior.

## Module boundaries

| Component | Responsibility |
| --- | --- |
| `lib/plan.h`, `lib/plan.c` | Derive the driver certificate from the completed operation order and retain it in the prepared plan. |
| `lib/frozen.h`, `lib/index.c` | Open the exact key's cursor, position toward the real delta start with bounded progress, count and locate identifiers without exposing mutable storage. |
| `lib/cost.c`, `lib/cost.h` | Share pure execution-cost arithmetic between candidate estimation and the scheduler's measured-driver suffix estimate. |
| `lib/exec.h`, `lib/exec.c` | Run prefix preparation and bounded suffixes through the same dispatcher; preserve cursor and lower-level state across every yield; observe actual driver visits. |
| `lib/round.h`, `lib/round.c` | Own prepared-item records, charge preparation to scheduling, construct all ordered tasks before publication, and commit or discard prefix counts and notes with the round. |
| `lib/task.c` | Consume the task's whole-plan, empty or prepared-slice form; retain continuations while filling bounded candidate packets. |
| `lib/eval.c` | Record serial driver/suffix observations and keep the selected plan on fallback. |
| `lib/merge.c` | Consume unique candidates in logical position order and include prefix scratch peaks in the ordinary admission floor. |

The task's execution form identifies which startup contract applies. A
prepared slice always names an eligible item's driver; an ordinary task
has no prefix record to consume. A scan-specific `cut` flag cannot express
these different startup contracts. Explain still describes
the selected operations; dynamic task boundaries need no public option or
new explain syntax. The existing serial and forced-deferred modes are the
references.

## Failure paths and limits

Preparation, prefix execution, task execution and candidate collection all
precede publication. A failure to allocate control arrays or function
scratch, a refused scratch charge, a prefix error, an interrupt during
seek/count/cutting, or an ordinary worker/merge refusal declines the entire
round. Partially filled task arrays are not executable: the published job
is constructed only after all items and slices are complete.

Before serial fallback, the caller joins any published job, releases every
round-scoped charge and candidate buffer, discards prefix records and
worker counters, and invalidates prepared cursors. Valid ordinary frontier
movement can remain: it changes neither the tuple sequence nor counted
evaluation work. Fallback uses the already selected prepared plans and
does not reconsider candidates using a partially executed round.

Index headers cannot grow, relations cannot append and no clear or discard
can occur between cursor preparation and the worker barrier. Workers read
the store through incomplete const types in `lib/frozen.h`; new cursor
helpers preserve that boundary. Task starts and fences are internally
constructed identifiers, checked against their operation's view before
use. Key hashes retain exact equality checks; slicing neither trusts a
hash collision nor prints key contents in an error.

Once task execution ends, cursor metadata is no longer used; the existing
merge owns reservation and mutation. Limit refusals and partial allocator
reservation retain the parallel design's existing guarantees. Slicing
adds no exception to value identity, visible diagnostics, callback thread
affinity or the guarantee that a failed gathered query delivers no rows.
Streaming queries remain on the calling thread.

## Determinism

Let `S` be the ordered driver sequence and `J(x)` the serial suffix
derivations under the binding produced by visiting identifier `x`. If
residual matching rejects `x`, `J(x)` is empty. The prefix either rejects
everything or runs once before:

```text
J(S[0]) ++ J(S[1]) ++ ... ++ J(S[M-1])
```

Ordinal boundaries divide this concatenation without permuting or
duplicating an element. The frozen-store rule makes each `J(x)` independent
of when other tasks run. Ordering unique tuples by their minimum
task/emission positions therefore gives the serial derivation order with
only duplicates removed. The routing and merge proofs then apply
unchanged to tuple identifiers, secondary chains, pending-value numbering,
storage capacities and output order.

The same argument partitions the driver's and suffix's read counts.
Adding the prefix once completes counter equivalence. Body-position view
assignments are unchanged, so this also preserves the semi-naive proof that
each derivation belongs to exactly one variant. It does not rely on uniform
keys, contiguous matching tuple identifiers, equal runtime per task or
successful allocation in the deferred attempt.

## Validation

### Mechanism and semantic regressions

The first regression exercises a substantial constant-key indexed join
with default scheduling. It checks that explain retains the two lookups,
the round creates multiple nonempty driving slices, and the deferred run
merges successfully. A zero parallel-round result or one whole indexed
task fails the mechanism check even if the answer is correct. Keep the
fixture outside query factoring so that another optimization cannot remove
the scheduling problem.

A compact fixture for the `pair` rule has 3,000 distinct `row(A, V, 7)`
facts and eight distinct `row(B, V, 13)` facts per `V`. It has 3,000
driving matches, 24,000 result tuples and 27,000 evaluated lookup visits.
The query's streaming scan is separate. These known counts distinguish
driver work from output cardinality and detect either repeated or omitted
visits without a timing assertion.

| Area | Required cases and assertions |
| --- | --- |
| Sequence partition | Dense and sparse identifier chains; zero, one and many matches; every legal piece count; more workers than matches; ordered concatenation exactly equals the unsliced visits. |
| Residual matching | Repeated variables within the driver, constants, anonymous terms and mixed-type domains; rejected tuples still count once. |
| Views | All four views; matches before, at and after both boundaries; empty old/delta/current views; nonlinear and mutual recursion; late-acquired indexes. Suffix views stay unchanged. |
| Frontiers | Many slices share one delta key; starts are not published as frontiers; cancelled positioning and an exhausted key followed by appended matches remain valid. |
| Prefix | True and false constant membership/absence/existence tests; zero-arity predicates; constant comparisons, mixed-type notes, string functions and errors. Prefix reads occur once even with no driver matches. |
| Eligibility | Assignment or aggregate before the generator stays one whole task; no snapshot of a number or text binding; seeds and generator-free plans remain valid. |
| Executor yields | Rejection stretches past a quantum; pause at a slice's final identifier; full and pending-value batches at a boundary; repeated resumes never reset the cursor or backtrack into the prefix. |
| Ordering | Duplicates across slices and items; new integers and strings shared across workers; exact tuple/value identifiers, value-use bits, notes, output and successful work counters at each thread count. |
| Skew | One heavy key among many short keys, clustered tuple IDs, one heavy suffix binding, and expensive suffixes under a small driver. Driver counts and suffix estimates remain distinct. |
| Refusal | Allocation injection in prepared-item and task growth, prefix text scratch, candidates and merge; tuple/memory limits; interruption in seek, count, cut and execution; serial fallback and complete charge release. |
| Lifetime | End a round, append and grow indexes, then prepare the next round; clear/discard between evaluations; no cached pointer or slice survives the frozen interval. |
| Arithmetic | Counts near identifier limits, many items, maximum thread count, saturated estimates and byte-size overflow; no wrapped boundary or allocation. |

Use `tests/unit/test_index.c` for bounded storage walks and sentinel/frontier
invariants, `test_round.c` for task construction, ownership and refusal,
`test_eval.c` and language cases for execution equivalence, and
`tests/perf.sh` for stable scheduling and preparation-work bounds. Add
coverage to `test_cost.c` for measured-driver overrides and saturation.
Small exhaustive and randomized sequence models are useful independent
oracles, but do not replace the executor and failure-injection cases.

Compare the same indexed plan at one worker, several worker counts and
forced deferral, including one worker in a threads-disabled build.
`-X no-index` checks answer equivalence separately; its tuple-read counts
are intentionally different. Naive execution checks recursive answers;
it is not expected to match semi-naive derivation counts. Vary task claims
and delayed workers without sleep-based correctness assertions. A
thread-enabled integration fixture confirms useful background work using
pool instrumentation; portable unit tests assert the partition and merge
even when only the caller is available.

Internal measurements distinguish driver matches, setup links, slices,
task work and completed merges. After a normal or failed attempt they
must not be confused with public statistics. For `P > 1`, total boundary
and count work is linear in `M`, with the documented separate positioning
cost `F`; doubling workers must not multiply it by the chain length.
Control storage follows item and worker counts when `M` grows.

### Performance acceptance

Use the [benchmark measurement rules](../docs/todo-benchmark-performance.md#measurement-and-acceptance-rules).
Isolate data loading, index construction, prefix/driver preparation,
task execution, candidate screening, merge and output. Keep equivalent
binaries, flags, datasets and output sinks; an end-to-end improvement
from bulk import is not evidence that the cursor split pays for itself.

Measure `facts-join` and selective joins at scales 1, 2, 4 and 8 and at
1, 4, 8, 12 and 16 workers. Include a large driver with a cheap suffix,
a rare key, a small driver with large fan-out, a duplicate-heavy projection,
recursive shrinking deltas and clustered skew. Record accepted and
declined preparation costs, user CPU, peak RSS, task imbalance and live
candidate bytes as well as wall time. Compare identical selected plans
and check successful output, storage and semantic counters exactly.

Default scheduling is accepted only when indexed work reaches multiple
workers, setup stays within its documented bounds, and isolated execution
improves without a repeatable unexplained regression on controls. The
second traversal and the first count on a declined round are explicit
costs that measurements must justify. The combined bulk-load and slicing
result is judged against the competitive target for the complete command.
Candidate collection and serial merge work remain separate constraints;
more scheduled tasks alone is not a performance result.

## Alternatives not taken

* **Replace the lookup with a scan to obtain tasks.** It discards selectivity
  and changes public tuple-read counts. Access-path choice and task
  partitioning are separate decisions.
* **Materialize all matching identifiers.** It makes slicing convenient
  but adds `O(M)` data storage and a new limit-sensitive allocation beside
  the input, candidates and output. Boundary walks need only `O(P)` records.
* **Maintain per-key lengths or skip directories on insertion.** A length
  alone does not locate ordinal boundaries or measure a restricted view.
  Persistent navigation metadata taxes every index and insert, including
  workloads that run serially. It needs independent evidence beyond the
  existence of a sliced driver.
* **Cut numeric identifier ranges equally.** Matching identifiers can all
  lie in one range. Dividing match ordinals balances driver visits even
  for sparse and clustered chains.
* **Have every slice seek from the head or its own delta start.** This can
  cost `O(P*M)` links and violates the shared frontier's single-start
  contract if those starts are published. Prepared starts avoid both.
* **One-pass adaptive landmarks.** Bounded coarsening can approximate
  match-ordinal boundaries, but adds state and variable piece sizes.
  Two linear link passes give exact counts and balanced boundaries with
  a simpler proof. Their cost is measured before adding another scheme.
* **Replay the constant prefix on every task.** It multiplies reads,
  allocation attempts and notes. Executing it once and bounding the
  runnable suffix preserves the serial behavior by construction.
* **Snapshot an arbitrary binding prefix.** Assignments and folds can own
  text or computed values. Copying their lifetime and backtracking state
  broadens the executor contract without helping a constant-key driver.
* **Split a live inner join or steal continuations.** It requires a global
  ordering of nested binding positions and transferable executor state.
  Fixed driver slices retain the existing ordered-task merge and make
  their remaining single-binding granularity limit explicit.
