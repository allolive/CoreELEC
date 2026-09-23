#!/usr/bin/env python3
"""Unpack the pinned Kodi and apply every patch, the way a build would.

The result is the tree the build compiles, so the tests run against what
ships rather than against a reconstruction. A patch that no longer applies,
or one of ours that only applies with fuzz, stops this here.
"""

import argparse
from pathlib import Path
import sys

from native_source import prepare_tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="where to unpack the tree")
    parser.add_argument("--offline", action="store_true",
                        help="fail rather than download the archive")
    args = parser.parse_args()
    sys.dont_write_bytecode = True
    applied = prepare_tree(args.out, download=not args.offline)
    ours = sum(1 for _, yacer, _ in applied if yacer)
    fuzzy = [(path, lines) for path, _, lines in applied if lines]
    print(f"{len(applied)} patches applied, {ours} ours")
    for path, lines in fuzzy:
        print(f"  fuzz: {path.name}: {lines[0].strip()}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
