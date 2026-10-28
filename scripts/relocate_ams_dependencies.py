"""Relocate CMake/pkg-config metadata in the official Windows dependency bundle.

Only text metadata under the explicitly supplied dependency prefix is changed;
library binaries, headers, and the installed Bambu Studio are left untouched.
"""

import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", type=Path, help="Extracted bundle's usr/local directory")
    args = parser.parse_args()
    prefix = args.prefix.resolve(strict=True)
    if not (prefix / "include").is_dir() or not (prefix / "lib").is_dir():
        parser.error("Expected a dependency prefix containing include/ and lib/.")
    original = "D:/a/BambuStudio/BambuStudio/deps/build/BambuStudio_dep/usr/local"
    changed = 0
    for path in prefix.rglob("*"):
        if path.suffix not in (".cmake", ".pc") or not path.is_file():
            continue
        text = path.read_text(encoding="utf-8")
        if original not in text:
            continue
        path.write_text(text.replace(original, prefix.as_posix()), encoding="utf-8")
        changed += 1
    print(f"Relocated {changed} metadata files to {prefix}")


if __name__ == "__main__":
    main()
