# Working agreement

## Collaboration

* Ask, don't assume. If something is unclear, ask before writing a single line.
  Never make silent assumptions about intent, architecture, or requirements.
* Flag uncertainty explicitly. If you are not confident about an approach or
  technical detail, say so before proceeding. Confidence without certainty
  causes more damage than admitting a gap.
* I'm always open to ideas on better ways to do things. Please don't hesitate
  to suggest a better way, or one that has long lasting impact over a tactical
  change.
* Security is paramount and on equal footing with correctness.
* **If you discover a bug, discrepancy, or something unexpected, fix it**
  whether or not it relates to the task at hand. Fix it if it is
  straightforward or required to make progress; otherwise document it in
  `TODO.md`.
* Read this file again after each context compaction.

## Dependencies

relic has no third-party runtime dependencies, and that is a product
requirement: the tool must be as easy to install and audit as `awk` or `grep`.
Follow the [dependency policy](specs/architecture.md#dependency-policy). The
sole optional dependency is `libedit`, used only for REPL line editing.

Development tools are a separate question. If a build or development tool is
missing, install it via homebrew or ask me to install it. Do not work around a
missing tool.

## Design

* Code should be simple and clean, never over-complicate things.
* **Judge complexity by the model a maintainer must understand.** Compare the
  concepts, entities, independently varying states, ownership rules and
  coordination obligations a change introduces with those it removes across
  the whole system. Lines of code are a useful proxy for review effort, not
  an acceptance gate. A larger module is a win when it replaces scattered
  mechanisms with one clear owner and fewer rules for its callers. Moving
  the same obligations behind a new interface does not remove them. Follow
  the [architectural complexity guidance](specs/style.md#architectural-complexity).
* **Human readability is a requirement.** Follow the
  [readability standard](specs/style.md#readability-standard): another
  developer must understand purpose, data flow, invariants and failure paths
  without reconstructing the implementation or reading the conversation.
* If a design starts getting too complicated or convoluted, or accumulates a
  lot of edge cases, stop and re-evaluate the big picture. A pile of special
  cases is usually a symptom that the architecture is wrong: step back and ask
  whether changing the architecture would make the edge cases disappear rather
  than handling them one by one. A simpler design that eliminates a class of
  problems beats a clever one that patches each instance.
* **Open-ended enumeration is the smell; "by construction" is the fix.** The
  warning is a correctness argument that depends on remembering every caller,
  execution path or exception that must participate in a protocol. Each new
  caller can invalidate that argument. An explicit enum or exhaustive switch
  over a closed set of states can instead make a design easier to understand;
  spelling out necessary cases is not itself a defect. When review keeps
  finding missing participants or exceptions, stop adding entries. Ask what makes
  the enumeration necessary at all, and change that instead, so the property
  holds *by construction* rather than by exclusion: make the illegal state
  unrepresentable, make the invariant a derived fact nothing can destroy, make
  the one funnel the only way to reach the operation. Two questions get you
  there quickly -- "what is this list protecting?" and "what would make the
  list empty?" Prefer the answer that deletes mechanism over the one that adds
  it; a fix that is wider but shallower (many mechanical call-site edits, each
  failing loudly if wrong) beats one that is narrow but deep (a small clever
  interlock whose failures are schedule-dependent).

## Code

* Follow the [style guide](specs/style.md). It covers layout, naming, `_t`
  typedefs, comments, the BSD memory primitives, and `getopt(3)` flags.
* Watch the argument order of `bcopy(src, dst, n)`, the opposite of `memcpy`.
  Flag it in code review.
* Do not leave temporal or campaign-specific marks in code, comments, doc
  strings, specs or user-facing docs: no "now fixed", "previously broken",
  "as of <date>", "Wave 3", "Addressing BUG-12", "(fixed in commit abc123)".
  Code and docs describe the current state of the system; the why-and-when
  belongs in commit messages, which are read in temporal context.
* Source files (`.c`, `.h`, `Makefile.am`, `configure.ac`, `m4/*.m4`, shell
  scripts) must be ASCII-only; CI fails the build on any byte > 0x7f.
  Markdown prose is exempt.

## Tests and sanitizers

* Write a detailed test suite as you add features. It must be re-executed at
  every major change.
* When fixing a non-trivial bug, write a regression test FIRST that exercises
  the broken path and confirm it actually fails against the unfixed tree. Only
  then apply the fix and re-run the test to confirm it now passes. A fix
  without a failing-then-passing test is not a fix - it is a guess that happens
  to compile. Trivial typo and one-line obvious-correct fixes are exempt;
  anything involving the evaluator, the Magic rewrite, memory ownership, or a
  multi-step code path is not trivial.
* `make check-asan` rebuilds with AddressSanitizer and runs the full suite with
  the strict `ASAN_OPTIONS` set in `Makefile.am` (leaks, use-after-return and
  init order fail the run; allocations may return NULL so relic's failure
  handling is exercised). Treat any ASan report as a HIGH finding and fix the
  root cause. Mandatory before any commit touching allocation, relation or
  index storage, or query result lifetime.
* `make check-ubsan` runs the suite under UndefinedBehaviorSanitizer. Run it
  alongside `check-asan` when touching arithmetic or bit-level code.
* `make check-tsan` runs the suite under ThreadSanitizer with threads
  enabled. Run it when touching parallel evaluation or anything shared
  between workers.
* `make check-lsan` (standalone LeakSanitizer, Linux) and `make check-msan`
  (MemorySanitizer, clang on Linux) run where available and are no-ops
  otherwise.

## Committing

* Each solid progress should be committed in the git repository.
* Before committing code, test that what you produced is high quality and that
  it works:
  - Launch a code-review subagent **FIRST**, before running tests. Ask it to
    enumerate bugs, design issues, AND security / information-leak concerns.
    Frame the security pass explicitly in the prompt - don't assume the agent
    will check on its own.
  - Ask the reviewer to apply the style guide's
    [readability review](specs/style.md#readability-review). Require specific
    functions or passages as evidence; formatting, comment count, and passing
    tests are insufficient. Address readability findings before committing.
    For an architectural change, require a before/after comparison of the
    concepts and maintenance obligations added and removed across the system;
    a line-count delta alone does not establish simplification.
  - Address every CRITICAL and HIGH finding before committing. Note MEDIUM
    findings in `TODO.md` or the commit message if not fixed.

## Documents

[specs/README.md](specs/README.md) indexes the design documents and states
their conventions. Besides `specs/`, [TODO.md](TODO.md) is the forward-looking
index of open work and [NEWS](NEWS) says what changed in each release.

* For every major feature, write `specs/design-<feature>.md` first and
  reference it from `specs/architecture.md` before implementation.
* **Specs describe the target design, in the present tense.** Edit them when
  the design changes, not to record what shipped, what was deferred, or which
  parts are not built yet - that belongs in `TODO.md`. A design doc is never
  trimmed to match a partial implementation.
* A decision that constrains more than one subsystem is recorded, with its
  rationale, in the [contracts](specs/architecture.md#contracts) or the
  pipeline entry of the architecture; any other decision lives in the design
  doc it shapes. When a
  decision changes, edit it in place; git holds the history.
* `TODO.md` is split into categories (major initiatives, bugs by subsystem,
  security residuals, planned features). Each item has a `[Severity]` tag
  (Critical / High / Medium / Low) and a component pointer. Small items live
  inline as one- to few-line bullets. Larger initiatives fan out to category
  docs in `docs/todo-*.md` and are referenced from `TODO.md` by a
  one-paragraph summary. Each category doc follows the same rule recursively:
  small items inline, anything beyond about a page splits into its own file
  linked from the category doc. When an item is closed, delete it - git
  history is the record of completed work.
* `TODO.md` holds only work worth doing. Do not add an item for something
  not worth fixing, or for behaviour a man page or design doc already states
  as a contract or limitation. A limitation users should know about belongs
  in the user-facing docs (README, man pages), not in `TODO.md`.
