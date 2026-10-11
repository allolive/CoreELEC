#!/usr/bin/env python3
"""Find what a hand-resolved merge leaves behind inside patch files.

Reads each patch's hunks by their line counts, so a patch's own file headers
are never mistaken for content: inside a hunk, an added line that is a conflict
marker, or that is itself a diff header (a reject's `+++ b/...` copied into the
source), is reported. Exits non-zero when anything is found.

  tests/check-patch-leftovers.py FILE...
"""

import re
import sys

HUNK = re.compile(rb"^@@ -\d+(?:,(\d+))? \+\d+(?:,(\d+))? @@")
# conflict markers, and diff headers with or without the + or - a careless
# copy out of a reject keeps
BAD = re.compile(rb"^(<<<<<<< |>>>>>>> |=======$|\+\+\+? [ab]/|---? [ab]/|\+\+\+ |@@ -\d)")


def leftovers(path):
    found = []
    old = new = 0
    with open(path, "rb") as fh:
        for number, raw in enumerate(fh, 1):
            line = raw.rstrip(b"\r\n")
            if old > 0 or new > 0:
                kind, body = line[:1], line[1:]
                if kind == b"\\":                # "\ No newline at end of file"
                    continue
                if kind in (b" ", b""):
                    old -= 1
                    new -= 1
                elif kind == b"-":
                    old -= 1
                elif kind == b"+":
                    new -= 1
                    if BAD.match(body):
                        found.append((number, line.decode(errors="replace")))
                else:                            # the hunk ended short of its count
                    old = new = 0
                continue
            match = HUNK.match(line)
            if match:
                old = int(match[1]) if match[1] is not None else 1
                new = int(match[2]) if match[2] is not None else 1
    return found


def main():
    bad = 0
    for path in sys.argv[1:]:
        for number, line in leftovers(path):
            print(f"::error file={path},line={number}::merge leftover: {line[:120]}")
            bad += 1
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
