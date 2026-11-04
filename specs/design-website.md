# Project website

The public website introduces relic and teaches its command-line and C
interfaces. GitHub Pages serves the contents of `website/` at
`https://stass.github.io/relic/`. Four hand-written HTML pages share one
stylesheet: the landing page, quickstart, language tutorial and C API overview.
Relative links work both under the project path and in a local preview.

The site uses system fonts, readable line lengths, visible navigation and
ordinary code blocks. It requires no JavaScript, downloaded fonts, analytics,
site generator or package installation. Navigation remains visible on small
screens; code blocks scroll without widening the page. Keyboard navigation,
visible focus and a skip link are part of every page.

The manual pages and language specification remain the reference contracts.
The website explains a small set of runnable examples and links to those
references rather than duplicating their full detail. Changes to documented
syntax or interface contracts update the corresponding website passages.

The Pages workflow checks local links, fragments and page structure on pull
requests and pushes affecting the site. Deployment runs only from `master`
or a manual run on `master`, with write permissions limited to the deployment
job. Only `website/` is uploaded; repository files and build products are not
published. Actions are pinned to commits. Repository Pages settings select
GitHub Actions as the publishing source.

`python3 tests/check-website.py` checks the static site. With `--relic`, it
also runs each complete Datalog example against that executable and compares
its sorted output with the example's declared answer. The C example is
compiled against librelic when editing the API page. A local preview uses
`python3 -m http.server --bind 127.0.0.1 --directory website 8000`.
