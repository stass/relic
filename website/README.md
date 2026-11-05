# Website maintenance

The public site consists of six HTML files, the shared `assets/style.css`,
and stylesheets and JavaScript modules for the playground and performance page. Edit these sources
directly. The repository's manual pages and language
specification define the contracts explained by the site.

Build the optional Wasm and benchmark-data assets, then preview from the repository root:

```sh
make website-assets
python3 -m http.server --bind 127.0.0.1 --directory website 8000
```

Open `http://localhost:8000`. All site links are relative, so they also work
under the `/relic/` GitHub Pages prefix.

The guides work without a Wasm build. The playground imports the reusable
[JavaScript API](../wasm/README.md); programs and results stay on the device.
Generated assets under `assets/wasm/` are ignored by git and excluded from
source distributions. Do not commit them. The Pages workflow builds them
from its checkout with a pinned Emscripten SDK before uploading the site.

The performance page renders SVG charts in the browser from recorded benchmark
data. Refresh that small generated snapshot without rebuilding Wasm with:

```sh
python3 bench/compare/tools/export-website.py
python3 tests/test_website_benchmarks.py
```

`assets/benchmark-data.json` is also ignored by git and excluded from source
distributions. Deployment regenerates it from `bench/compare/results.tsv`;
no benchmarks run during page loading or deployment. The source report in
`BENCHMARKS.md` defines the hardware and measurement context described by the
performance page. Update that prose when publishing a different cohort.

Check page structure, local links and fragments:

```sh
python3 tests/check-website.py
```

After building relic, also execute the complete Datalog examples:

```sh
python3 tests/check-website.py --relic ./relic
```

Complete examples carry a `data-answer` attribute containing their expected
sorted output, including its final newline. When editing `c-api.html`, compile
its complete C example against the built or installed library and run it;
its answers are `b` and `c`.

Run the optional C-in-Wasm and JavaScript API checks, then the real-browser
suite (pass a Chrome, Chromium or Brave executable):

```sh
make wasm-check WASM_OUTPUT="$PWD/website/assets/wasm"
node tests/wasm/browser-tests.mjs /path/to/chrome
```

The browser test runner uses Node's built-in HTTP, WebSocket and process APIs;
it starts an isolated profile and loopback server and cleans them up after the
run. The tests cover API limits, typed values, cancellation and recovery, as
well as the playground examples and literal DOM rendering.

In GitHub repository Settings > Pages, select **GitHub Actions** as the
publishing source. `.github/workflows/pages.yml` checks pull requests and
publishes `website/` on matching pushes to `master`. It also supports manual
dispatch from `master`. The public URL is `https://stass.github.io/relic/`.
