* Ask, don't assume. If something is unclear, ask before writing a single line. Never make silent assumptions about intent, architecture, or requirements.
* Flag uncertainty explicitly. If you are not confident about an approach or technical detail, say so before proceeding. Confidence without certainty causes more damage than admitting a gap.
* I'm always open to ideas on better ways to do things. Please don't hesitate to suggest a better way, or one that has long lasting impact over a tactical change."
* If dependencies are not available, install them via homebrew or ask user to install -- do not try to work things around.
* Prefer a good third-party library over reimplementing external functionality.
  Use an installed dependency when it can be used unchanged. When you need
  library code changes or custom allocator integration, vendor a pinned upstream
  release instead of maintaining a custom external package. Retain the license,
  provenance and checksums; keep integration separate from upstream source where
  possible, document patches, and build it under the applicable sanitizers.
  See specs/architecture.md Section 5.6 for the dependency policy.
* Code should be simple and clean, never over-complicate things.
* **Human readability is a requirement.** Write code so another developer can
  understand its purpose, data flow, invariants and failure paths without
  reconstructing the implementation or reading the conversation. Before writing
  nontrivial code, read analogous libbfx functions and follow the
  [readability standard](specs/style.md#readability-standard). Make the main flow
  read as a sequence of named operations in the problem's vocabulary. Keep
  representation details in cohesive helpers, name values by their role and
  units, and make mutation and failure handling explicit. Explain the algorithm
  and its invariants in enough detail to understand why it works. Comments and
  blank lines do not repair a function whose structure is hard to follow.
* Prefer functional style: separate pure computation from code with side
  effects where practical. Create reusable functions without side effects
  when doing so does not add complexity.
* If a design starts getting too complicated or convoluted, or accumulates a
  lot of edge cases, stop and re-evaluate the big picture.  A pile of special
  cases is usually a symptom that the architecture is wrong: step back and ask
  whether changing the architecture would make the edge cases disappear rather
  than handling them one by one.  A simpler design that eliminates a class of
  problems beats a clever one that patches each instance.
* **Enumeration is the smell; "by construction" is the fix.**  The concrete
  tell that the bullet above applies is finding yourself writing a *list*:
  the callers that must participate, the paths that can run inside a
  window, the states a value can be in, the cases a check must cover.  A
  design whose correctness rests on such a list is only ever as good as the
  list, and the list is usually short -- each review round adds one more
  entry and the entry is never the last.  When that happens, stop adding
  entries.  Ask what makes the enumeration necessary at all, and change
  that instead, so the property holds *by construction* rather than by
  exclusion: make the illegal state unrepresentable, make the invariant a
  derived fact nothing can destroy, make the one funnel the only way to
  reach the operation.  Two questions get you there quickly -- "what is
  this list protecting?" and "what would make the list empty?"  Prefer the
  answer that deletes mechanism over the one that adds it; a fix that is
  wider but shallower (many mechanical call-site edits, each failing loudly
  if wrong) beats one that is narrow but deep (a small clever interlock
  whose failures are schedule-dependent).
* Follow the [style guide](specs/style.md). Give structs
  matching `_t` typedefs (e.g. `typedef struct data_fill data_fill_t;`) and use
  those typedefs. Separate logical code blocks with blank lines.
* Each solid progress should be committed in the git repository.
* Before committing, you should test that what you produced is high quality and that it works:
  - Launch a code-review subagent **FIRST**, before running tests. Ask it to enumerate
    bugs, design issues, AND security / information-leak concerns. Frame the
    security pass explicitly in the prompt — don't assume the agent will check on
    its own. 
  - Ask the reviewer to apply the style guide's
    [readability review](specs/style.md#readability-review) and compare the code
    with the selected examples. Require specific functions or passages
    as evidence; formatting, comment count, and passing tests are insufficient.
    Address readability findings before committing.
  - Address every CRITICAL and HIGH finding before committing. Note MEDIUM
    findings in TODO.md or the commit message if not fixed.
* For every major feature, write a design document in `specs/` first
  (`specs/design-<feature>.md`) and reference it from `specs/architecture.md`
  before implementation. This keeps decisions reviewable and durable.
* **Architecture and design docs are forward-looking and describe the
  long-term shape of the system, not the current implementation status.**
  `specs/architecture.md` and every `specs/design-*.md` capture the target
  design we are building toward.  Only edit them when the long-term
  architecture or design itself changes (a new subsystem, a reversed
  decision, a renamed primitive).  Do **not** rewrite them to record
  what shipped in a given phase, what was deferred for scheduling
  reasons, or which parts of the design are not yet implemented — that
  belongs in `STATUS.md` (current snapshot), `TODO.md` (forward-looking
  index of open work), or the per-initiative migration plan
  (e.g. `bfd-improve-plan.md`).  If a phase ships a subset of a design
  doc, the design doc stays unchanged and the deferral is recorded in
  the plan and TODO.md.  Trimming a design doc to match a partial
  implementation erodes the durable architecture record and forces
  future readers to reconstruct intent from commit history.
* Write a detailed test suite as you add more features. The test must be re-executed at every major change.
* When fixing a non-trivial bug, write a regression test FIRST that exercises
  the broken path and confirm it actually fails against the unfixed tree.
  Only then apply the fix and re-run the test to confirm it now passes.  A
  fix without a failing-then-passing test is not a fix — it is a guess that
  happens to compile.  Trivial typo / one-line obvious-correct fixes are
  exempt; anything involving concurrency, on-disk format, recovery, or a
  multi-step code path is not trivial.
* Comment logical blocks concisely only where their purpose or reasoning is
  not obvious. Comments must be self-contained and self-explanatory: no
  references to design documents, temporal marks, or narration of the code.
  Use complete explanations for non-obvious algorithms, representations and
  contracts; do not compress them into cryptic one-line comments. Remove
  redundant words, not the reasoning a maintainer needs.
* Do not leave temporal or campaign-specific marks in code, comments, commit
  bodies that ship inline (e.g. doc strings), or user-facing docs.  No
  "now fixed", "previously broken", "as of <date>", "Wave 3 changes",
  "Addressing BUG-12", "post-rebase", "after the security campaign",
  "(fixed in commit abc123)", or similar.  Code and docs describe the
  current state of the system; the why-and-when belongs in commit
  messages, which are versioned and read in temporal context.
  Inline temporal references rot the moment the next change lands and
  mislead future readers.
  - Run `make check-asan` periodically.  This reconfigures with
    `--enable-asan --enable-test-hooks`, rebuilds, and runs the full
    suite under AddressSanitizer.  The `check-asan` target exports
    `ASAN_OPTIONS` as a colon-separated list:
        halt_on_error=1
        detect_leaks=1
        detect_stack_use_after_return=1
        check_initialization_order=1
        strict_string_checks=1
        detect_invalid_pointer_pairs=2
        abort_on_error=1
    so leaks, use-after-return, dynamic-init-order fiascos, and a
    poisoned read all fail the test instead of silently logging.
    ASan on macOS defaults to `halt_on_error=0` and `detect_leaks=0`,
    so the explicit options matter.  When ASan fires, treat it as a
    HIGH finding and fix the root cause before committing.
    Mandatory before any commit touching memory allocation,
    return-buffer ownership, or cursor lifecycle.
  - Run `make check-lsan` when LeakSanitizer is available
    standalone (clang/gcc on Linux; on macOS LSan ships only as part
    of ASan and is exercised by `check-asan` above).  Cheaper than
    full ASan, useful for a quick leak smoke before push.
  - Run `make check-msan` when MemorySanitizer is available
    (clang on Linux with an MSan-instrumented libc++; not supported
    on macOS or FreeBSD).  MSan catches reads of uninitialized
    memory that ASan does not.  `./configure` auto-detects MSan
    support; if absent, `make check-msan` is a no-op.  Best-effort
    on platforms that have it; not a precondition for commits on
    platforms that lack it.
  - `make check-ubsan` (UndefinedBehaviorSanitizer) runs the suite
    with `-fsanitize=undefined` and
    `UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`.  Catches
    signed overflow, alignment violations, and shifts past width.
    Cheap; run alongside `check-asan` when touching arithmetic or
    bit-level code.
* Prefer the BSD memory primitives from `<strings.h>` (note the
  trailing `s`) when they fit:
      bzero(buf, n)         instead of memset(buf, 0, n)
      bcopy(src, dst, n)    instead of memcpy(dst, src, n)
      bcmp(a, b, n) == 0    instead of memcmp(a, b, n) == 0
  All three are available on FreeBSD, macOS, and Linux glibc.
  WATCH the argument order on `bcopy` - it is `(src, dst, n)`,
  the opposite of `memcpy`.  This is the classic foot-gun; flag
  it in code review.  Reach for `mem*` whenever the BSD version
  does not fit: `memset` for non-zero fills, `memcmp` when you
  need a sign (lexicographic ordering), `memcpy` when call-site
  types make `bcopy`'s reversed args harder to read.  No
  annotation comments are required - just pick the function that
  reads cleanest.
* CLI flags follow BSD style: single-letter options parsed with
  `getopt(3)` (not `getopt_long`).  Avoid `--long-arguments` entirely.
  When a knob has many variants (resource limits), use a single
  letter that takes a `key=value` argument and is repeatable
  (e.g. `-l depth=128 -l nodes=2000000`), not a separate `-X` per
  knob.
* Source files must be ASCII-only.  No em-dashes, smart quotes,
  arrows, ellipsis characters, non-breaking spaces, or any other
  byte > 0x7f in `.c`, `.h`, `.peg`, `Makefile.am`, `configure.ac`,
  `m4/*.m4`, or shell scripts.  CI greps the source tree for
  non-ASCII bytes and fails the build.  Markdown under `specs/`
  and `docs/` is exempt because it is prose, not source.
* Read this file again after each context compaction.
* Security is paramount and on equal footing with correctness
VERY IMPORTANT: if you discover a bug or discrepancy or something unexpected, we should fix it whether it is related to the current issue or not.  Fix it if it's straightforward or required to make progress or document it in TODO.md otherwise.

Project status and open work live in two root-level files:
- `STATUS.md` — current state of the system: what's implemented,
  what's not, architectural tradeoffs.  Treat it as a snapshot, not
  a roadmap.  Keep it accurate as features land.
- `TODO.md` — high-level forward-looking index, split into
  categories (major initiatives, bugs by subsystem, security
  residuals, planned features).  Each item has a `[Severity]` tag
  (Critical / High / Medium / Low) and a component pointer.  Small
  items live inline as one- to few-line bullets.  Larger
  initiatives fan out to category docs in `docs/todo-*.md` and are
  referenced from TODO.md by a one-paragraph summary.  Each
  category doc follows the same rule recursively: small items
  inline, anything that grows beyond ~1 page splits into its own
  file linked from the category doc.  When an item is closed,
  delete it — git history is the record of completed work.

### Specs

### Completed architecture

### Open work
See TODO.md.
