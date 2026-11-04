# Website maintenance

The public site consists of four HTML files and `assets/style.css`. Edit
these files directly. The source repository's manual pages and language
specification define the contracts explained by the site.

Preview from the repository root:

```sh
python3 -m http.server --bind 127.0.0.1 --directory website 8000
```

Open `http://localhost:8000`. All site links are relative, so they also work
under the `/relic/` GitHub Pages prefix.

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

In GitHub repository Settings > Pages, select **GitHub Actions** as the
publishing source. `.github/workflows/pages.yml` checks pull requests and
publishes `website/` on matching pushes to `master`. It also supports manual
dispatch from `master`. The public URL is `https://stass.github.io/relic/`.
