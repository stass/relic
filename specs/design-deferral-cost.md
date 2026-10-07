# relic deferral cost design

**Status.** Design document for the cost of a deferred round over a serial
one on a single thread: where the extra time goes, what part of it the
design of deferral requires and what part is representation, and the
changes that take the second part out. Forward-looking, like every
document under `specs/`; `TODO.md` records what is not yet implemented.

**Companions.** `specs/design-parallel.md` sections 5.3, 5.4, 6.1, 6.3 and
8 for the screen, the threshold, the merge and the accounting this
document measures and amends; `specs/design-relations.md` section 6 for
the batch insertion whose shape the screen takes; `specs/design-memory.md`
sections 5 and 6 for the growable arrays and the identifier table;
`specs/design-evaluation-cost.md` section 6 for the batch;
`docs/todo-parallel.md` for the gaps at sixteen threads that the same
cost explains.

---

## 1. What this document settles

* what a deferred round does that a serial round does not, counted per
  derivation the frozen store holds, per derivation it lacks, per new
  tuple and per round, with the measurements that sorted the four;
* that the floor under a deferred round is proportional to its **new
  tuples** and not to its derivations or to the number of rounds: a round
  that inserts a third of what it derives costs 2.6 times its serial
  self on one thread, a round that inserts one tuple in five hundred
  costs one per cent more;
* that the worker's screen is probed one dependent cache miss at a time
  where the store it stands in for is probed in overlapping passes, and
  that it is rebuilt from sixteen slots every round, moving each
  candidate 1.5 to 1.8 times as it grows;
* that the merge walks every candidate once per stripe and makes some
  seventy-five allocations a round, each sized by a count it already
  knows;
* three changes, in the order they are measured: the screen is probed as
  the store is probed; a round's buffers are sized once, from what its
  items inserted last time; the merge walks each stripe's own candidates
  and takes its scratch in one piece;
* what remains after them, which is the price of finding a tuple several
  workers derived and of staging an append, and how the threshold of
  `specs/design-parallel.md` section 5.4 is re-measured against it.

It changes nothing a program can observe. Every relation, counter,
diagnostic and storage byte comes out as the serial evaluator leaves it,
at every thread count, which the identity tests of
`specs/design-parallel.md` section 16 hold.

---

## 2. Where the time goes, measured

Three workloads of `bench/compare` at scale 1, without output (`-n`), on
one thread, run serially and with every round deferred under
`-X parallel`, `--disable-shared` build at `-O2`, five runs each, user
seconds; the machine of `specs/design-parallel.md` section 2 with other
work on it, so the figures are read for their ratios:

| Workload | Serial | Deferred | Rounds | Derivations | Inserted |
| --- | --- | --- | --- | --- | --- |
| `tc-cyclic` | 2.93 to 3.08 | 3.51 to 3.64 | 4 | 49,775,727 | 1,000,000 |
| `cspa` | 0.22 to 0.24 | 0.24 to 0.25 | 30 | 2,887,364 | 256,296 |
| `chain-closure` | 0.44 to 0.46 | 0.65 to 0.70 | 2,001 | 4,002,000 | 2,001,000 |

Twenty per cent, eight and fifty. The three shapes differ in what they
insert: one tuple in fifty, one in eleven, one in two.

**Per round.** A build that counts, in each deferred round, the
derivations screened, those the frozen store lacked, the candidates kept
and the entries the screen's growth moved, and times each round's task
phase and merge, gives for `tc-cyclic`:

| Round | Derivations | Store lacked | Candidates | Moved | Serial ms | Deferred ms | of it tasks | of it merge |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 0 | 48,727 | 48,727 | 48,727 | 49,140 | 5 | 9 | 4.5 | 4.0 |
| 1 | 2,376,242 | 2,256,676 | 863,571 | 1,572,852 | 224 | 582 | 470 | 110 |
| 2 | 42,082,256 | 3,653,332 | 87,702 | 98,292 | 2,320 | 2,344 | 2,334 | 9 |
| 3 | 4,268,502 | 0 | 0 | 0 | 219 | 203 | 203 | 0 |

The whole difference is round 1, the recursive rule's first round, which
inserts 864 thousand of the 2.4 million tuples it derives: its task phase
alone takes twice the serial round, insertions included, and its merge
half as long again. Round 2 derives eighteen times as much and pays one
per cent: its 3.65 million derivations the store lacks are repeats of
88 thousand new tuples, each probed in a screen of 88 thousand entries
that fits in the second-level cache. Round 3 is faster deferred, since it
inserts nothing and screens as the serial evaluator probes.

`chain-closure` runs 2,001 rounds of about a thousand new tuples each.
Its task phase, 373 ms in all, costs what the serial rounds cost
insertions included, 404 ms; its merge adds 303 ms, 151 microseconds a
round, 150 nanoseconds a tuple. `cspa`: 30 rounds, tasks 191 ms against
209 serial, merge 36 ms, 139 nanoseconds a tuple. The merge costs between
100 and 150 nanoseconds per new tuple on every workload and every round
measured, so it is a cost per tuple and not per round.

**Sampled.** A profile of `tc-cyclic` deferred against serial, 2,746
samples against 2,458, attributes the 288 extra to: the probe of the
worker's screen, inlined into `screen_batch()`, about 300 samples of self
time plus 100 more in `relic_idprobe_next()`; the screen's growth,
`candidate_rehash()`, `relic_idtab_place()` and
`relic_idtab_complete_growth()`, 76; the merge, `find_new_in_stripe()`,
`relic_relation_apply_append()` and `relic_relation_stage()`, 64. Against
that, `relic_relation_screen()` costs 200 samples less than the insertion
it replaces, since it inserts nothing. In `chain-closure`, 718 samples
against 488, the growth is 70 of the 230, `find_new_in_stripe()` 49, and
`candidates_reserve()` with the capacity arithmetic it calls 45; the
store's own tables growing, `relic_idtab_complete_growth()` hashing every
tuple again, is a quarter of the serial run and the same in both.

**Prototyped.** Three toggles in a scratch build, each configuration run
five to seven times interleaved on a loaded machine and the fastest
kept, in milliseconds of the task phase:

| Configuration | `tc-cyclic` round 1 | `chain-closure`, all rounds |
| --- | --- | --- |
| as built | 536 | 389 |
| screen slots prefetched before probing | 514 | 394 |
| no accounting arithmetic when nothing grows | 503 | 366 |
| screen and arrays sized before the round | 441 | 319 |
| all three | 375 | 296 |

The merge did not move: 119 to 133 ms for round 1, 308 to 325 ms for the
chain. Sizing the screen before the round is the largest single gain and
removes the growth entirely; prefetching and the fast path each take
several per cent. The sized screen was given the round's exact count,
which a real round only has from history, section 5.

---

## 3. The cost model

A deferred round does what a serial round does -- runs the executor,
probes the canonical index for every derivation, appends and indexes the
new tuples -- and the following besides, by `lib/task.c` and
`lib/merge.c`:

**Per derivation the frozen store holds: nothing.** `screen_batch()`
asks the worker's screen only for a tuple the store lacks, and the store
is probed by `relic_relation_screen()`, which is the insertion's own two
passes without the insertion.

**Per derivation the store lacks: one probe of the worker's screen.** A
dependent chain of three cache misses on a hit -- the slot, then the
candidate's entry for its target and offset, then its columns -- and of
one on a miss, since the screen is tagged. Nothing is prefetched; the
batch of 64 tuples whose store probes overlapped now waits on each screen
probe in turn. Round 1 of `tc-cyclic` pays this 2.26 million times into a
screen growing to two million slots; round 2 pays it 3.65 million times
into a warm screen and hardly notices. The serial evaluator pays the
first probe only, since its earlier insertion put the tuple in the store.

**Per new tuple: the entry, and the growth that makes room for it.** A
place in the screen, which walks the probe sequence the failed probe just
walked; `candidates_reserve()`, which projects the bytes of seven arrays
through their growth policies on every call, growth or not; the copy of
the columns into the worker's buffer. Then in the merge: a probe and a
place in a stripe's table of firsts, the copy into the target's staging
with its key hashes, a probe and a place in a table of new keys per index,
and the canonical place and the index link the serial insertion also
makes. Six random memory accesses and two copies beyond the serial
insertion's, which is what 150 nanoseconds buys.

**Per round: starting from nothing.** The screen starts at sixteen slots
and doubles through every candidate count on its way, moving every
entry at each doubling: 1.57 million moves for 864 thousand candidates,
1,524 for a thousand, and each move reads an entry far from the table to
hash it again. The entry, stripe and column arrays double alongside,
reallocating. The merge opens a table of firsts and an array of
references per stripe, a table of new keys and an array per part of each
index, three arrays per target and three per secondary index for the
staging, each charged to the account and allocated on its own, some
seventy-five allocations and as many atomic charges and releases a
round. And each of its parallel phases walks the whole candidate or
staging array once per stripe, reading a byte and taking a branch that
goes one way in sixteen, so that on one thread each array is read sixteen
times over in three phases.

Of these, three are what deferral is. A tuple several workers derive is
gathered by each and resolved by the merge, which is the choice that
keeps workers from sharing a table (`specs/design-parallel.md` section
4.3), so the table of firsts stays. The staging is what gives the new
tuples their identifiers in serial order before any part applies them,
so its copy stays. And a worker that could not screen its own repeats
would buffer every derivation, so the screen stays. Everything else is
representation: how the screen is probed, when its room is made, how the
merge finds a stripe's candidates and takes its scratch. Sections 4 to 6
change those four, and section 7 says what is left.

---

## 4. The screen is probed as the store is probed

`screen_batch()` has, when `relic_relation_screen()` returns, the wide
hash of every tuple of the batch and which the store holds. The tuples
the store lacks are then screened in the passes the store's own probe
makes, so that their misses overlap:

1. for each, prefetch the screen slot its screen hash selects
   (`relic_idtab_prefetch()`), as `relic_relation_insert_batch()` does
   for the canonical index;
2. for each whose slot holds a candidate of its tag, read the position
   the slot holds (`relic_idtab_peek()`, which in a tagged table gives
   nothing for a slot of another tag) and prefetch that candidate's
   entry;
3. for each such candidate, prefetch its columns at the entry's offset,
   which is the comparison a hit makes;
4. probe, and add what the screen lacks.

Three passes over at most 64 tuples, each a loop over addresses already
computed, and the fourth is today's loop. A tuple whose slot is empty
or holds a candidate of another tag, which the probe passes over unread,
takes the first pass only, as a new tuple almost always does: a slot
taken at all, which at the screen's loads of three eighths to three
quarters is a third to a half of them, is taken by another tag 255
times in 256. The passes read nothing the probe
does not read and change nothing; a prefetch is a hint, and a wrong one
costs a line.

A tuple the screen lacks is placed by `relic_idtab_place()`, which walks
the probe sequence again from the hash. The walk the failed probe just
made could be kept and its slot used, but `specs/design-memory.md`
section 6 has insertion re-probe from the hash so that no caller need
know a growth invalidated its cursor, and here a growth can come between
the probe and the place; the table is hot by then and the second walk
costs a few per cent, which is not worth an exception to that rule.

**Entries and columns stay apart.** The second and third passes are
the ones the store does not need, since its slot names the tuple's
columns directly; they exist because a candidate's columns are reached
through its entry. A buffer laid out as the store is -- one array of
records of the round's widest arity, header and columns together, so
that a position names a record by multiplication -- would make a hit two
misses like the store's, and would cost a round whose targets differ in
arity the difference per candidate. It is not built. The second pass
asks for the entry a pass ahead of the probe, overlapping its miss with
the batch's others, which is the wait the record would remove. On round
1 of `tc-cyclic`, interleaved and the fastest of nine runs each, the
first pass alone takes the task phase to 0.90 of its unprefetched time
and the three passes to 0.86. How much of what remains a hit spends
waiting on its entry that measurement does not separate; a profile that
shows the probe waiting on entries is what reopens the record
(section 11).

---

## 5. A round's buffers are sized once

The screen's growth is the largest cost of section 2's prototype, and it
is a cost of starting every round from sixteen slots. The buffers cannot
stay from one round to the next: they hold data, are charged while they
exist, and a buffer kept uncharged between rounds would hold memory the
limit could not see, while one kept charged would make what the limit
refuses in a later serial round depend on whether an earlier round was
deferred, which `specs/design-parallel.md` section 12 forbids. So they
are released at every round's end as today, and made once at its start,
at the size the round is expected to need.

**The estimate.** Beside a plan's history of the work its last run did,
an item keeps a record of the tuples that run **inserted**
(`relic_insert_history_t`): on the serial path the difference in the
context's inserted count across the item's turn, on the deferred path
the firsts the merge counted for the item's tasks. The record belongs to
what derives the tuples rather than to the plan that ran: the two plans
of a rule's delta variant, of which a round runs the one its delta's
size selects, derive the same tuples and share one record, so a round
that crosses from one plan to the other is still predicted. It is
recorded unscaled and used unscaled: the new tuples of a recursive
component's rounds do not follow its delta -- round 2 of `tc-cyclic`
reads eighteen times round 1's delta and inserts a tenth as much -- and
the previous round's count predicts the next within a small factor,
where a count scaled by the delta would predict fifteen million for 88
thousand. An item that has not run is expected to insert nothing: its
first view bounds neither what it derives nor what of that is new, and a
rule that reads a hundred million tuples to keep ten would reserve
gigabytes on that view's word. The round's **expected candidates** are
the sum over its items. An item keeps its records on every context whose
rounds may be deferred, `-X parallel` on one thread among them, so that
the single-worker deferred round reserves as the pool's does.

**The reservation.** Before the job is published, each worker's
candidates are given room for the expected candidates when the pool has
one worker, and for twice its share of them otherwise, since a worker
claims whatever tasks it reaches: the screen reserved for that many
entries, the entry and stripe arrays grown to that many candidates, and
the column array to that many at the expected candidates' width, their
columns at each target's arity summed and divided by their count,
rounded up. Growth stays as it is for a round that exceeds its estimate;
it then starts from the estimate rather than from nothing, which for a
round predicted within a factor of two is one doubling. The reservation
is charged as a growth is today, through `candidates_reserve()`, so the
account sees the same bytes it would have seen at the round's end. A
reservation the limit or the allocator refuses declines the round before
anything runs, as a refused text buffer does (`begin_worker_texts()`),
and the round runs serially.

A refused reservation is counted on the pool, beside the rounds merged
and abandoned, so that the memory limit's sweep can require that it
reaches one.

**What it costs.** A round predicted ten times too large -- round 2 of
`tc-cyclic`, reserved from round 1's 864 thousand -- charges 20 MB on one
thread, 1.3 MB per worker on sixteen, for a round that needs a tenth of
it, and gives it back at the round's end. The serial evaluator would
hold none of it, so near the memory limit the deferred round is declined
where the serial one would run, and runs serially: the outcome
`specs/design-parallel.md` section 6.1 already provides, paid in time
and never in a different answer. A round predicted too small pays a
shorter cascade, and an item that has not run pays the whole of it. The
round that costs most today, the first round of a recursive rule, is such
an item and keeps its cascade: `docs/todo-parallel.md` wants an estimate
from the plan for the same round, and when one exists it is a bound on
derivations, too loose to reserve by; the reservation stays with what was
inserted.

**Nothing is projected when nothing grows.** `candidates_reserve()`
first asks whether the arrays hold what the growth adds -- each array's
count against its capacity, the screen's room (`relic_idtab_has_room()`)
-- and returns at once when they do, computing the projection only when
one must grow, as `relic_idtab_reserve()` already does for its table.
With the reservation above that is every add but a handful.

---

## 6. The merge walks its own candidates and takes its scratch once

**Each stripe's candidates are listed.** Once the merge has resolved
the pending values, which can move a candidate to another stripe, each
worker's candidates are sorted by position into the stripes of their
targets' canonical indexes -- a counting sort, whose counts are the
`by_stripe` counts the worker keeps, writing one position per candidate
into an array of its candidate count -- a worker to a part on the
pool. A stripe's part of the merge then walks task by task the
positions of its own stripe in the task's worker's list, each worker's
list being in derivation order and a task's candidates a contiguous run
of it; the walk finds each task's run by a search for the task's first
position, so that it reads exactly the task's candidates whatever order
the workers ran their tasks in. The same is done for a target's staging
once it is staged: a list of staged positions per canonical stripe, and
per secondary index per key stripe, built by one counting sort per
target and index, a part each. Phase 1 reads each candidate once
instead of sixteen times; the classification and the application read
each staged tuple once per index instead of sixteen times per index.

This reverses the choice of `specs/design-parallel.md` section 6.3 that
every part reads every candidate's stripe, a byte each, in place of a
pass that copies into buckets. The pass that is added copies four bytes
per tuple, once, in parallel, and is sequential in what it reads and
writes; what it removes is fifteen of every sixteen reads of the merge's
three parallel phases, which on one thread is the merge's largest piece
and on sixteen is a fifteen-sixteenths cut in what each part reads.

**A step's scratch is one allocation.** Every count a step of the
merge sizes its scratch by is known before the step begins, from the
steps before it: each worker's candidates for its list and each
stripe's for its table of firsts and references, once the candidates
are gathered; each target's new tuples for its staging and its lists,
once they are counted; each part's tuples for its table of new keys,
once the staged tuples are listed. Such a step sums its counts into one
block, charges it once, allocates it once and carves it by part
(`relic_block_piece()`, measuring and carving with one layout); a part
opens nothing, frees nothing and cannot fail. The block of the firsts
is released once they are counted and that of the new keys once the
keys are classified, so neither is held beside the other, nor at the
admission of `specs/design-parallel.md` section 6.1; a target's staging
block, from `relic_relation_append_open()`, is held until the merge
ends. One block cannot serve the whole merge, since the new keys' counts
are known only once the staging they classify exists. A block holds
every part's tables at once, for as long as its step runs, so the
round's peak counts all of them together; near the memory limit a merge
may be declined that tables made and freed part by part would have
fitted, and its round runs serially, slower and the same. The tables of
firsts and of new keys take their slots from their block, a form of the
identifier table that places its slots in memory given to it
(`relic_idtab_init_given()`), and each part makes its table there when
it begins, so that the parts clear their tables in parallel. A round's
merge makes two allocations and charges for its steps, and one per
target.

---

## 7. What this leaves, and the threshold

What a deferred round still pays over a serial one on one thread, per
new tuple: a probe and a place in the screen; the copy into the
candidate buffer; a probe and a place in a table of firsts; the copy
into the staging; a probe and a place in a table of new keys per index.
With the probes overlapped and the tables sized, these are the cost of
the memory traffic they make, which on `tc-cyclic`'s round 1 is some
tens of nanoseconds per new tuple on each side. Per derivation the store
lacks: a screen probe overlapped with its neighbours'. Per round: three
allocations and a barrier.

**The expected result** on one thread, from the prototype and the
profiles: `tc-cyclic` deferred within a tenth of serial, from a fifth,
since its round 1 keeps most of its cascade; `chain-closure` within a
sixth, from a half; `cspa` within a few per cent. Section 10's gates are
set from measurement on a quiet machine and recorded with each step; the
figures here are what the measurement is expected to confirm, not the
gates.

**The threshold.** `RELIC_PARALLEL_MIN_WORK` was set when a round that
inserts most of what it derives gained nothing from the pool, and
`docs/todo-parallel.md` already records that the parallel merge has
changed that. The cost this document removes is the other half of the
same calibration: it is the floor the threshold protects a round from,
and with the floor lower the threshold is re-measured, at the chain
lengths and `tc-cyclic` named there, once sections 4 to 6 are in. The
cost model of section 3 is what to measure against: a round's deferred
time is its serial time less its insertions, divided by the workers,
plus its new tuples at the merge's rate, plus the barrier; a round is
worth deferring when that is less than its serial time, and the
threshold is where the two cross for the rounds a workload has. A
threshold that weighs the expected new tuples beside the expected work
follows from that model; whether it pays over a single bound is what the
measurement says, and this document does not decide it.

---

## 8. Limits, accounting and determinism

Nothing a limit refuses changes. The reservation of section 5 charges
before the round what today's growth charges during it, the same holder
of `specs/design-parallel.md` section 8, released at the round's end as
today; a reservation refused is a round declined before publication,
which is a round run serially, where the limit judges every insertion as
it always has. The blocks of section 6 are charged to the round while
they are held, and the admission question of section 6.1 asks with the
staging blocks in the total and without the blocks of the firsts and of
the new keys, which are released before it asks. The storage total
after a merge is the serial evaluator's byte for byte: the scratch is
not storage.

Nothing a program derives changes. The passes of section 4 read what the
probe reads; the lists of section 6 visit the same candidates in the
same order, task by task and within a task in derivation order, and the
firsts they find are the firsts found today. Every identity test of
`specs/design-parallel.md` section 16 runs unchanged and must pass
unchanged at every thread count.

---

## 9. Tests

* **Identity.** The golden suite under `-X parallel` at one, two, four
  and sixteen threads, as today; output, counters and storage bytes
  identical to the serial run. This is the proof for every change here.
* **Allocation failure.** The sweep that injects failure into every
  allocation runs over the reservation of section 5, the block of
  section 6 and the placing constructor of the identifier table: each
  refusal declines the round, the round runs serially, the answer is the
  same and `relic_leave()` finds every charged byte returned.
* **The identifier table.** A unit test for a table in memory given to
  it against one that allocates its own, slot for slot, over the same
  sequence of keys.
* **The lists.** A unit test that the counting sort of a worker's
  candidates lists each stripe's positions in increasing order, every
  position once, for candidates of several targets and arities.
* **History.** A test that an item's recorded insertions equal what the
  serial run inserted for it, on the serial path and on the deferred
  one, through a component whose rounds grow and then shrink.
* **Performance.** `bench/run.sh` with `BENCH_THREADS="1 16"` and the
  three workloads of section 2 under `-X parallel` at `-j 1`, recorded
  with each step of section 10. `tests/perf.sh` bounds nothing new:
  there is no counter for what this document changes, and the design
  adds none, since none is a fact about the program.

---

## 10. Order of work

Each step is a commit with its tests and its measurement, on a quiet
machine, against the step before it; the figures of section 2 are the
baseline for the first.

1. **The screen probed in passes, and the fast path** (sections 4 and
   5's last paragraph). `lib/task.c`, and the identifier table's peek,
   which passes over a slot of another tag. Gate: identity; round 1 of
   `tc-cyclic`'s task phase faster by the prototype's margin or more; no
   workload of `bench/run.sh` slower at one thread without `-X parallel`,
   since the serial path does not run this code.
2. **Buffers sized once** (section 5). The history's `inserted`, the
   reservation, the decline. Gate: identity; the allocation-failure
   sweep over the new refusal; `chain-closure` under `-X parallel` at
   `-j 1` with its task phase no slower than its serial rounds; the
   screen's moves, counted in a debug build, zero on every round of
   `chain-closure` after the first and on rounds 2 and 3 of `tc-cyclic`.
3. **The merge's lists and block** (section 6). `lib/merge.c`,
   `lib/relation.c`'s append, the identifier table's placing
   constructor. Gate: identity; `chain-closure`'s merge below 100
   microseconds a round from 150; `tc-cyclic` at `-j 16` under
   `-X parallel` no slower than before, since the lists trade sixteen
   parallel reads for one sort.
4. **The threshold** (section 7). Re-measure `RELIC_PARALLEL_MIN_WORK`
   with the chains and `tc-cyclic` of `docs/todo-parallel.md`; record
   the constant and the measurement; close that page's item on the
   threshold and amend section 5.4 of `specs/design-parallel.md` with the
   figures.

Steps 1 to 3 are independent in what they change and are ordered by
what each is measured against; step 4 depends on all three.

---

## 11. What is deliberately absent

* **Buffers kept from one round to the next.** Section 5: they hold
  data, and the account's rule that data-sized memory is charged while
  it exists is what makes the limit the same at every thread count. The
  reservation gives a round the one allocation it would have reached
  anyway, and gives it back.
* **A screen that forgets.** A fixed-size screen owned by the pool, as
  the task array is, would need no sizing and no charge, and a repeat it
  had evicted would reach the merge as a candidate, which finds it. But
  the candidates would then be bounded by the eviction rate and not by
  the distinct new tuples, and round 2 of `tc-cyclic` -- 3.65 million
  repeats of 88 thousand tuples -- would gather millions where it
  gathers 88 thousand. The bound of `specs/design-parallel.md` section
  5.3 is kept.
* **Skipping the table of firsts when the pool has one worker.** Every
  candidate is then a first by construction, and the table costs
  44 samples of `tc-cyclic`. The single-worker deferred round exists to
  test the merge that runs with many, and a merge that took a different
  path alone would test something else. Section 6 makes the table's walk
  cheap for every worker count instead.
* **Tables that remember the hash.** A slot holding its key's 32-bit
  hash beside the identifier rehashes without reading the entry it
  names, which would make the screen's cascade several times cheaper and
  the relation store's as well, where it is a quarter of `chain-closure`.
  `specs/design-memory.md` section 6 measured that slot on the closure
  workloads and kept the identifier alone: the duplicate checks that
  dominate them are successful searches, which gain nothing from it, and
  the doubled slot array raised the peak memory. The cascade measured
  here is new evidence on the same question, recorded in `TODO.md`, and
  this document does not reopen it. Section 5 removes the cascade where
  it can be predicted rather than making it cheaper where it cannot.
* **Probing the screen before the store.** The screen is smaller and
  often warmer, but the store holds the answer for 48 of every 49
  derivations of `tc-cyclic`, and a screen probe before each of those
  would cost more than it saves on the forty-ninth. The store is asked
  first, and the screen only when it says no.
* **Candidates laid out as the store's records.** Section 4: the passes
  ask for the entry a hit reads before the columns ahead of the probe,
  and a round whose targets differ in arity would pay the widest for
  every candidate.
* **A screen shared by the workers.** It is the concurrent table
  `specs/design-parallel.md` section 4.3 keeps out.
* **A counter for what this changes.** The screen's moves, the probes
  overlapped and the allocations made are facts about a build, not about
  a program, and `-X stats` reports the latter.

---

## 12. Amendments this design makes

* `specs/design-parallel.md` section 5.3: the batch is screened in
  passes with prefetching, section 4, and the worker's buffers are
  reserved from the round's expected candidates before the job is
  published, section 5; section 5.4: an item records the tuples it
  inserted, and the threshold is re-measured against the
  floor of section 7; section 6.3 phase 1 and phases 4 and 7: a part
  walks its stripe's listed positions rather than every candidate's
  stripe byte, section 6; section 8: the buffers are charged at the
  round's start for their reservation and each step of the merge takes
  its scratch as one charged block; section 15: the measurements of this
  document's section 10 join the gates; section 18: "a screen that
  forgets" and "tables that remember the hash" are listed as absent.
* `specs/design-memory.md` section 6: the identifier table gains a form
  whose slots lie in memory its owner gives it.
* `specs/architecture.md`: the parallel evaluation section points here
  for the cost of a deferred round.
* `TODO.md`: the item on a deferred round's cost points here; the items
  of `docs/todo-parallel.md` on the threshold and on rounds that insert
  what they derive are resolved by steps 3 and 4 of section 10.
