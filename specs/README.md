# relic design documents

`specs/` holds the design of relic: what the system is, why it is built the
way it is, and the invariants a contributor must keep. These documents
describe the target design -- the long-term shape of the system -- rather
than the code line by line; the code, its comments and `include/relic.h`
carry the detail, and the manual pages relic(1) and relic(3) carry the
user-facing contract.

## Reading order

A newcomer should read:

1. [Architecture](architecture.md), the map: goals, principles, the
   pipeline from source text to answers, and the contracts every subsystem
   keeps.
2. [The relic language](language.md), the language relic accepts and what
   a program means.
3. The foundations -- memory, values and storage -- which every later
   stage builds on.
4. The pipeline documents, in the order listed below, and the interface
   and extension documents, as far as the work at hand needs. Each opens
   with a summary that says where it sits.
5. Before changing code, testing and code style; benchmarks and releases
   when the work touches performance or a release.

## Documents

**Overview**

* [Architecture](architecture.md) -- the system as a whole: goals and
  non-goals, principles, pipeline, query modes, cross-cutting contracts,
  source layout.
* [The relic language](language.md) -- syntax, types, semantics, safety,
  stratification, diagnostics and limits of the language.

**Foundations**

* [Memory](design-memory.md) -- arenas, growable arrays, the failure
  contract and the memory account.
* [Values](design-values.md) -- interning, domain identifiers, the symbol
  pool and value tables, the hash seed.
* [Storage](design-storage.md) -- relations, the canonical index,
  secondary indexes, deltas and frontiers.

**Pipeline**, in the order a program and its queries pass through it

* [Front end](design-frontend.md) -- lexer, parser, error recovery, the
  check, predicate identity and safety.
* [Facts](design-facts.md) -- base stores, the all-or-nothing commit, and
  when derived relations are current.
* [Bulk fact construction and import](design-bulk-load.md) -- transferable
  staging payloads, ordered value publication and bounded parallel imports.
* [Analysis](design-stratify.md) -- the dependency graph, components,
  strata and the evaluation schedule.
* [Magic Sets](design-magic.md) -- the rewrite that specializes a query's
  program to its bindings, its limit and its fallback.
* [Bound query factoring](design-query-factoring.md) -- certified
  right-linear recursion, correlated demand propagation and base answers.
* [Planning](design-planning.md) -- plans, the greedy planner, access
  paths and existence checks; the `-e` output format is specified in its
  [Explain](design-planning.md#explain) section.
* [Recursive plan selection](design-recursive-planning.md) -- structural
  alternatives for delta variants, estimates from frozen views and index
  key counts, and deterministic selection at round boundaries.
* [Evaluation](design-eval.md) -- the executor, the naive and semi-naive
  fixed points, query answers, streaming and interrupts.
* [Expressions](design-expressions.md) -- arithmetic, string functions and
  the regular-expression matcher, and how computed values are interned.
* [Aggregates](design-aggregates.md) -- group rules, folding in place, and
  aggregates in strata and in the Magic rewrite.
* [Direct grouped reduction](design-group-reduction.md) -- certified
  producer reduction, reusable summaries, bounded partial groups and
  lookup-time aggregate outcomes.
* [Parallel evaluation](design-parallel.md) -- the worker pool, deferred
  and shared rounds over a frozen store, and the serial-order merge.
* [Ordered driving cursors](design-driver-slicing.md) -- bounded scan and
  index-chain slices, once-executed prefixes and preparation-aware scheduling.
* [Bounded candidate routing](design-round-routing.md) -- bounded producer
  packets, round-owned values and tuples, and ordered unique publication.
* [Pending value compaction](design-pending-values.md) -- compact private
  value references, deterministic resolution and merge workspace ownership.

**Interfaces**

* [Library interface](design-api.md) -- `<relic.h>`: contexts, ownership,
  callbacks, error classes, diagnostics and versioning.
* [Command line](design-cli.md) -- the tool's phases, output formats, exit
  status and interactive session.
* [Tables: import and export](design-tables.md) -- importing and
  exporting relations as csv, tsv, json and `dl` files.

**Extensions**

* [Floating point](design-floats.md) -- the floating-point type, designed
  so that it keeps determinism and one identifier per value.

**Engineering**

* [Testing](design-testing.md) -- the oracle, the equivalences, the random
  and fuzz suites.
* [Benchmarks](design-bench.md) -- benchmarking against other engines.
* [Releases](design-release.md) -- continuous integration and the release
  procedure.
* [Code style](style.md) -- the code style, binding on all relic-owned
  code.

## Conventions

**Target design, present tense.** Every document describes the system as
designed, in the present tense. It records no history: no "was", "now", "no
longer", campaign names or dates. A document changes when the design
changes, not when code lands, and is never trimmed to match a partial
implementation, since that would erase the record of intent.

**What goes elsewhere.** Work not yet done, including any part of a design
that is not built, belongs in [TODO.md](../TODO.md). What changed in a
release, for its users, belongs in [NEWS](../NEWS). Why and when something
changed belongs in commit messages.

**Design before code.** A subsystem gets its `design-<subsystem>.md`, and a
link from the [pipeline](architecture.md#pipeline) or the contracts of
architecture.md, before its code is written.

**Skeleton.** A design document has a title; one summary paragraph saying what
it covers and where it sits -- foundation, pipeline stage, interface, extension
or engineering; a one-line "Related:" list of links; then, where they apply and
in this order: Model (concepts and data flow), Invariants, the mechanism
sections with the tricky parts and why they work, Failure paths and limits,
Determinism, and Alternatives not taken (only those a contributor would
plausibly propose again). It names concepts and points at headers and sources
for structs, prototypes and constants rather than copying them.

**Links.** Refer to another document by a link to a named heading, such as
[stratification](language.md#stratification), never by a section number.
Headings are not numbered.

**Decisions.** A decision that constrains more than one subsystem is
recorded, with its rationale, in the
[contracts](architecture.md#contracts) of architecture.md or, for the
pipeline's own invariants, in its [pipeline](architecture.md#pipeline)
entry. A decision that shapes one subsystem lives in that subsystem's
design document. Either way it is stated once, with the reason for it and
the alternative it rejected, and edited in place when it changes; git
holds the history.
