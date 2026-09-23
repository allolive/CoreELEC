#!/usr/bin/env python3
"""Report gcov coverage of the production code the suites compile.

Only files from the patched Kodi tree count. Coverage of the tests themselves
says nothing, and coverage of googletest says less; both are dropped. The
number is a guide to what is untested, never a gate - a threshold only buys
assertion-free tests written to reach lines.

  cmake -S tests -B build -DKODI_SOURCE=<tree> -DYACER_COVERAGE=ON
  cmake --build build && ctest --test-dir build
  python3 tests/coverage.py --build build --source <tree> [--markdown]
"""

import argparse
from pathlib import Path
import re
import subprocess
import sys


def measure(build, source):
    """Line and branch coverage per production file, via gcov."""
    files = {}
    for data in sorted(Path(build).rglob("*.gcda")):
        report = subprocess.run(["gcov", "-b", "-n", str(data)],
                                capture_output=True, text=True, cwd=build).stdout
        for block in report.split("File '")[1:]:
            name = block.split("'", 1)[0]
            path = Path(name)
            if not path.is_absolute():
                continue
            try:
                relative = path.relative_to(source)
            except ValueError:
                continue
            # Staged third-party headers are not ours to cover.
            if str(relative).startswith(".yacer-include"):
                continue
            lines = re.search(r"Lines executed:([\d.]+)% of (\d+)", block)
            taken = re.search(r"Taken at least once:([\d.]+)% of (\d+)", block)
            if not lines:
                continue
            entry = files.setdefault(str(relative), [0.0, 0, 0.0, 0])
            # The same file can be compiled into several test binaries; keep
            # the best figure, which is what the suite as a whole reaches.
            if float(lines[1]) >= entry[0]:
                entry[0], entry[1] = float(lines[1]), int(lines[2])
                if taken:
                    entry[2], entry[3] = float(taken[1]), int(taken[2])
    return files


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", required=True, type=Path)
    parser.add_argument("--source", required=True, type=Path)
    parser.add_argument("--markdown", action="store_true", help="emit a table for a job summary")
    args = parser.parse_args()

    files = measure(args.build.resolve(), args.source.resolve())
    if not files:
        print("no coverage data; was the build configured with -DYACER_COVERAGE=ON?",
              file=sys.stderr)
        return 1

    total_lines = sum(count for _, count, _, _ in files.values())
    covered = sum(percent * count / 100.0 for percent, count, _, _ in files.values())
    total_branches = sum(count for _, _, _, count in files.values())
    branches = sum(percent * count / 100.0 for _, _, percent, count in files.values())
    overall = 100.0 * covered / total_lines if total_lines else 0.0
    overall_branches = 100.0 * branches / total_branches if total_branches else 0.0

    if args.markdown:
        print(f"### Coverage of the production code under test\n")
        print(f"{overall:.1f}% of {total_lines} lines, "
              f"{overall_branches:.1f}% of {total_branches} branches.\n")
        print("| lines | branches | file |")
        print("|---:|---:|---|")
        for name, (lp, ln, bp, bn) in sorted(files.items(), key=lambda item: item[1][0]):
            print(f"| {lp:.1f}% ({ln}) | {bp:.1f}% ({bn}) | `{name}` |")
        print("\nCoverage of the tests themselves and of googletest is excluded. "
              "This is a guide to what is untested, not a target.")
    else:
        for name, (lp, ln, bp, bn) in sorted(files.items(), key=lambda item: item[1][0]):
            print(f"  {lp:5.1f}% lines ({ln:4d})   {bp:5.1f}% branches ({bn:4d})   {name}")
        print(f"  {overall:.1f}% of {total_lines} lines, "
              f"{overall_branches:.1f}% of {total_branches} branches")
    return 0


if __name__ == "__main__":
    sys.exit(main())
