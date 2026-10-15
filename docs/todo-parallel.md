# Dense recursive memory: implementation and acceptance

[Medium, components: `lib/round.c`, `lib/task.c`, `lib/merge.c`, `lib/atomic.h`]

Implement and assess the [dense collection design](../specs/design-dense-rounds.md)
under the [performance work](todo-benchmark-performance.md). The old
[performance pass](../bench/compare/recorded/performance-pass.md) is closed.

## Prototype boundary

One target relation, two columns, and EMIT terms that name assigned values.
The finite value-domain rectangle must fit checked 32-bit cell positions.
Collection uses a fixed atomic rank array and one scalar sequence per worker.
It adds no per-worker tuple or bitmap copy, value namespace, shared growable
hash table, executor opcode, input cursor, second execution pass or CLI flag.
The existing serial evaluator and general collector remain the fallbacks.

Keep the implementation within **600 net runtime C/header lines**, including
integration and atomic wrappers. Use at most one collector module. Count
tests and diagnostic code separately. A budget miss requires a design review;
minifying code does not satisfy the readability requirement.

## Work sequence

1. Implement the rank collector and bounded stable ordering. Add tests for
   arbitrary task-to-worker assignments, a later task finishing first,
   repeated tuples, preexisting tuples, mixed assigned value types, empty
   tasks, integer bounds and source-schema rejection. The model is supporting
   evidence, not a substitute for C concurrency and ownership tests.
2. Feed ordered rows into existing append preparation, classification,
   reservation and application. Keep one all-or-nothing publication path.
   Exercise refusal and interrupt at initialization, gathering, compaction,
   ordering, staging and reservation; require ordinary serial retry and exact
   cleanup. Verify the default one-thread and thread-disabled paths.
3. In an isolated build, record phase capacities and selection on every round
   of cyclic scale 4. The second and third rounds must avoid worker tuple
   buffers; reducing only the largest round can leave an earlier peak.
   Record atomic attempts/successes, ordering work and abandoned-round replay
   in private diagnostics, never in timing binaries.
4. Run two alternating complete-command pairs for cyclic scale 4 at sixteen
   workers, with serial cyclic, acyclic, bound recursion and a large
   computed-value case as controls. Full exact answers, ordered hashes,
   counters and final storage precede timing. Keep source/build/input/output
   policies fixed and retain every observation.
5. Only a successful screen advances to ten paired observations on the target
   and affected controls, plus the supported-host comparison. Run full normal,
   ASan, UBSan and TSan suites and appropriate thread-disabled checks before
   adoption. Review correctness, design, security and readability before tests,
   as the working agreement requires.

## Decision

The target is **parallel memory amplification** in dense recursion. Aim for
at least a 90% reduction from the retained 8.45 GB peak and no repeatable
elapsed-time regression against the selected native engine. The rough
reference implies about 0.85 GB, close to twice Souffle's measured peak.
A fresh comparison must establish the actual ratio; the design's charged-byte
ledger is not an RSS prediction.

Reject a modest reduction that leaves multi-gigabyte staging, a speed loss,
an unexplained control regression, new public behavior, or dependence on
serial fallback. If it passes, report the remaining difference from Souffle
explicitly; reducing twenty times the rival's memory to roughly twice does
not mean exact parity. No general relation-store replacement is part of this
candidate.
