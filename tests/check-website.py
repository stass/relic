#!/usr/bin/env python3
"""Check the published directory; optionally execute its Datalog examples."""

import argparse
from html.parser import HTMLParser
from pathlib import Path
import subprocess
import sys
from urllib.parse import unquote, urlsplit


class Page(HTMLParser):
    def __init__(self, path):
        super().__init__(convert_charrefs=True)
        self.path = path
        self.ids = set()
        self.links = []
        self.examples = []
        self.example = None
        self.headings = 0
        self.current_links = 0
        self.has_title = False
        self.has_language = False
        self.has_viewport = False
        self.feed(path.read_text(encoding="utf-8"))
        self.close()

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            if attrs["id"] in self.ids:
                raise ValueError(f"{self.path}: duplicate id {attrs['id']}")
            self.ids.add(attrs["id"])
        for name in ("href", "src"):
            if name in attrs:
                self.links.append(attrs[name])
        if tag == "h1":
            self.headings += 1
        if tag == "a" and attrs.get("aria-current") == "page":
            self.current_links += 1
        if tag == "title":
            self.has_title = True
        if tag == "html" and attrs.get("lang") == "en":
            self.has_language = True
        if tag == "meta" and attrs.get("name") == "viewport":
            self.has_viewport = True
        if tag == "code" and "data-answer" in attrs:
            self.example = ["", attrs["data-answer"]]

    def handle_data(self, data):
        if self.example is not None:
            self.example[0] += data

    def handle_endtag(self, tag):
        if tag == "code" and self.example is not None:
            self.examples.append(self.example)
            self.example = None


def check_site(root, executable):
    pages = {path.resolve(): Page(path) for path in root.glob("*.html")}
    if not pages:
        raise ValueError(f"{root}: no HTML pages")
    examples = 0
    for path, page in pages.items():
        if (page.headings != 1 or page.current_links != 1 or
                not page.has_title or not page.has_language or
                not page.has_viewport or "main" not in page.ids):
            raise ValueError(f"{path}: missing page structure or navigation")
        for link in page.links:
            url = urlsplit(link)
            if url.scheme or url.netloc:
                if url.scheme != "https":
                    raise ValueError(f"{path}: unexpected external URL {link}")
                continue
            if url.path.startswith("/"):
                raise ValueError(f"{path}: link loses GitHub project prefix: {link}")
            target = (path.parent / unquote(url.path)).resolve() if url.path else path
            if root not in target.parents or not target.is_file():
                raise ValueError(f"{path}: missing or non-site target {link}")
            if url.fragment:
                if target not in pages or unquote(url.fragment) not in pages[target].ids:
                    raise ValueError(f"{path}: missing fragment {link}")
        if executable:
            for program, answer in page.examples:
                result = subprocess.run([executable, "-s"], input=program + "\n",
                                        text=True, capture_output=True, timeout=30)
                if result.returncode or result.stdout != answer:
                    raise ValueError(f"{path}: example failed\n{result.stderr}"
                                     f"expected {answer!r}, got {result.stdout!r}")
                examples += 1
    print(f"Checked {len(pages)} pages, local links and fragments; "
          f"executed {examples} Datalog examples.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--relic", help="path to a built relic executable")
    args = parser.parse_args()
    root = (Path(__file__).resolve().parents[1] / "website").resolve()
    executable = str(Path(args.relic).resolve()) if args.relic else None
    try:
        check_site(root, executable)
    except (ValueError, OSError, subprocess.SubprocessError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
