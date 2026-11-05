# Project website

The public website introduces relic and teaches its command-line, C and
browser interfaces. GitHub Pages serves the contents of `website/` at
`https://stass.github.io/relic/`. Hand-written HTML pages share a base stylesheet:
the landing page, quickstart, language tutorial, C API overview, playground and
[performance guide](design-performance-page.md).
Relative links work both under the project path and in a local preview.

Related: [WebAssembly interface](design-wasm.md),
[library interface](design-api.md), [language](language.md).

## Guides and navigation

The site uses system fonts, readable line lengths, visible navigation and
ordinary code blocks. The introductory guides require no JavaScript, downloaded
fonts, analytics, site generator or package installation. The performance page
uses JavaScript for charts and keeps its explanations and source links in HTML.
Navigation remains visible
on small screens; code blocks scroll without widening the page. Keyboard navigation,
visible focus and a skip link are part of every page.
Shared corner radii give code blocks, navigation, disclosures and feature cards
a consistent rounded appearance. Playground panels and controls use the same
sizes as the guides.

The manual pages and language specification remain the reference contracts.
The website explains a small set of runnable examples and links to those
references rather than duplicating their full detail. Changes to documented
syntax or interface contracts update the corresponding website passages.

## Playground

The playground is an opt-in JavaScript page using the public
`evaluate(program, options)` operation from the reusable Wasm distribution.
It does not contain another adapter, engine, result protocol or implementation
of resource limits. The module and its companion assets live under
`website/assets/wasm/`; the page imports them by relative URL. Guides link to
the playground without loading its scripts or Wasm.

A labeled textarea holds a complete relic program with embedded queries.
A small example selector supplies runnable examples, and Run and Stop control
one active operation. There is no editor framework, filesystem import UI or
server evaluation. Run clears the previous outcome and starts the reusable
operation with an `AbortController`. Stop aborts it. Controls expose the running
state, and a status region reports completion, failure or cancellation. A
subsequent Run creates a fresh operation and remains usable after any failure.

The page shows each query's rows separately in source order, bounded by the
API's output limits, and distinguishes no rows from a true zero-column answer.
A compact output panel follows the editor directly, before explanatory text,
with Run and Stop beside it on the right. The panel shows its empty, running
or failed state and scrolls long answers within the panel, keeping the results
close to the code.
The playground has its own stylesheet for the workspace frame, dark editor
and light output console. It uses system fonts and a plain textarea; visible
labels and focus indicators accompany the compact controls.
The renderer prints integers in exact decimal, symbols as identifiers and byte
strings with relic-compatible escaping, including NUL and non-UTF-8 bytes.
It does not substitute lossy string decoding for the library's typed values.
Diagnostics show severity and available line and column information. Successful
output identifies the engine version returned by the module. The page explains
that evaluation happens locally and that source, time, memory and output are
bounded; it reports exceeded limits instead of displaying incomplete answers
as complete ones.

Examples, source excerpts, results and diagnostics enter the DOM through
`textContent` or form values. None becomes HTML, a URL or executable code.
The page sends no source or results to a server, uses no CDN and saves no
program automatically. A visible fallback explains missing JavaScript or
required browser features and links to native installation instructions.

## Build and deployment

The Pages workflow checks local links, fragments and page structure on pull
requests and pushes affecting the site. Engine, compatibility, API, bridge
and build changes also rebuild and test the Wasm distribution with the pinned
SDK described in the [Wasm design](design-wasm.md#validation). The build script
accepts an output directory, so deployment places the complete artifact under
`website/assets/wasm/` and local builds can use a disposable directory.
Generated binaries and generated support modules are not committed.

Deployment runs only from `master` or a manual run on `master`, with write
permissions limited to the deployment job. Only `website/`, including the
validated Wasm distribution, is uploaded; unrelated repository files and build
products are not published. Actions are pinned to commits. Repository Pages
settings select GitHub Actions as the publishing source.

## Validation

`python3 tests/check-website.py` checks the static site. With `--relic`, it
also runs each complete Datalog example against that executable and compares
its sorted output with the example's declared answer. The C example is
compiled against librelic when editing the API page. Browser tests exercise
examples, typed result rendering, diagnostics, cancellation, timeout and
recovery through the public Wasm API. Hostile diagnostic and result text is
checked for literal rendering. A local preview uses
`python3 -m http.server --bind 127.0.0.1 --directory website 8000` after the
Wasm assets have been built into that directory.
