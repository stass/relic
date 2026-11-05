# Performance page

The performance page explains the recorded native benchmark comparison and the
methods used by the engine. Browser-rendered SVG charts explore the results;
the benchmark report and research index remain the evidence and provenance.
The page does not run benchmarks or evaluate submitted programs.

Related: [website](design-website.md), [benchmarks](design-bench.md),
[research index](../docs/research.md), [recorded results](../BENCHMARKS.md).

## Model

`bench/compare/tools/export-website.py` reads the canonical comparison TSV and exports
a compact JSON snapshot into the website's assets directory. Deployment builds
this snapshot from its checkout. It is a generated asset, ignored by git, and
contains scale-1 medians and explicit statuses for all recorded workloads at
one and sixteen threads. A source digest identifies the
input snapshot.

The page's JavaScript fetches that same-origin asset and renders SVG with
standard browser APIs. One chart presents the one-thread profile and another
presents the sixteen-thread profile, each with a corresponding timing table.
Both show all recorded scale-1 workloads. Rendering responds
to container width so labels remain readable on small screens. No charting
library, package installation, CDN, external font or analytics is required.

The guide remains useful without charts: measurement context, limitations,
technique descriptions and links to the complete recorded tables are ordinary
HTML. Loading failures retain this content and explain that charts could not
be loaded. A populated HTML timing table presents the values behind each
comparison, including the fastest completed rival.

## Invariants

* Medians come from successful recorded runs. Missing and failed measurements
  remain statuses and never become zero or invented timeout durations.
* Comparisons use one hardware cohort, scale and configured thread count.
* A positive speedup is reference median divided by relic median. Values
  above one favor relic, and values below one favor the reference.
* Charts, their descriptions and the timing table use the same named records.
  The page does not maintain a second set of timing constants.
* Display strings enter the DOM through text nodes, including SVG text and
  tooltips. Recorded labels never become markup or script.

## Charts and interpretation

Each chart shows relic relative to the fastest completed rival for
each displayed workload. A logarithmic ratio axis and a marked parity line
make both small differences and large ratios visible. Text identifies the
direction of a difference; color is supplementary. Hover descriptions and the
timing table name the reference engine and show the original elapsed medians.

Both charts include cases where a rival is faster. These are whole-command
comparisons, not an attribution of speedup to one implementation detail.

The page identifies processor, memory, operating system, repetitions and
compilation/output policy from `BENCHMARKS.md`, with a link to versions and
the complete measurement conditions. Its introduction summarizes the measured
results in plain language; detailed setup follows the charts. It distinguishes
native CLI measurements from the single-threaded Wasm playground. It links to
the complete results and raw timings.

## Research and implementation

Technique descriptions distinguish methods used by the runtime from related
work and proposals. Semi-naive evaluation, Magic Sets and eligible recursive
query factoring link to their method references and relic's owning designs.
The "Relic optimization techniques" section explains the work each technique
avoids and the conditions under which relic uses it, without attributing a
separate benchmark speedup to an individual optimization.
Storage, planning and deterministic parallel publication link to the local
contracts that describe them. Related structures that relic does not implement
are not advertised as implemented optimizations.

## Validation

Exporter tests cover invalid or duplicate measurements, preservation of
statuses and profile separation. Browser tests check both displayed
cohorts, ratio direction, timing-table agreement and narrow-screen
containment. Static checks cover navigation, local links and page structure.
Deployment regenerates the JSON before exercising the browser page.
