#!/usr/bin/env python3
"""Package the licenses of the optional toolchain runtime with its artifact."""

from pathlib import Path
import shutil
import sys


def main():
    root = Path(shutil.which("emcc")).resolve().parent
    # Homebrew's bin/emcc wrapper invokes the adjacent libexec installation.
    if (root.parent / "libexec" / "emcc").is_file():
        root = root.parent / "libexec"
    license_path = root / "LICENSE"
    if not license_path.is_file():
        license_path = root.parent / "LICENSE"
    notices = [
        ("Emscripten", license_path),
        ("musl libc", root / "system/lib/libc/musl/COPYRIGHT"),
        ("LLVM compiler-rt", root / "system/lib/compiler-rt/LICENSE.TXT"),
    ]
    text = "Runtime notices for the relic WebAssembly distribution\n\n"
    for name, path in notices:
        text += name + "\n\n" + path.read_text(encoding="utf-8") + "\n\n"
    text += ("dlmalloc is written by Doug Lea and released to the public domain "
             "under CC0: https://creativecommons.org/publicdomain/zero/1.0/\n")
    (Path(sys.argv[1]) / "THIRD-PARTY.txt").write_text(text, encoding="utf-8")


if __name__ == "__main__":
    main()
