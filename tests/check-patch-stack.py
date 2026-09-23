#!/usr/bin/env python3
"""Apply every package's patch stack to its pinned source, and report.

prepare-tree.py does this for Kodi, because that is the tree the tests
compile. The other packages are never apply-tested at all: a patch that has
stopped applying to common_drivers is found hours into a kernel build, or not
until CI runs. This applies all of them the way scripts/unpack does - same
order, same zero fuzz for ours - and says which stacks are clean.

It compiles nothing. Sources already cached by a build are reused; anything
missing is fetched unless --offline.
"""

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

from native_source import CORELEC, REPO

PROJECT = "Amlogic-ce"
PATCH_ARCH = "aarch64"


def packages():
    """Every package our branch patches, in the order the groups apply."""
    found = {}
    for path in sorted((REPO / "patches-yacer").glob("*/*/")):
        name = path.name
        if name.endswith("_tests") or not any(path.glob("*.patch")):
            continue
        found.setdefault(name, None)
    return sorted(found)


def package_dir(name):
    """The project override wins over the generic package, as the build does."""
    for root in (CORELEC / f"projects/{PROJECT}/packages", CORELEC / "packages"):
        for candidate in root.rglob(f"{name}/package.mk"):
            return candidate.parent
    return None


def declared(mk, key):
    match = re.search(rf'^{key}="([^"]*)"', mk.read_text(), re.MULTILINE)
    return match[1] if match else ""


def archive_for(name, directory, pin, download):
    cache = CORELEC / "sources" / name
    hits = [path for path in sorted(cache.glob(f"*{pin}*"))
            if path.suffix not in (".sha256", ".url")] if cache.is_dir() else []
    if hits:
        return hits[0]
    url = declared(directory / "package.mk", "PKG_URL").replace("${PKG_VERSION}", pin)
    url = url.replace("$PKG_VERSION", pin).replace("${PKG_NAME}", name)
    if not url or not download:
        return None
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / url.rsplit("/", 1)[-1].replace(pin, f"{name}-{pin}")
    handle, partial = tempfile.mkstemp(dir=cache, prefix=f".{name}-", suffix=".part")
    Path(partial).unlink()
    try:
        subprocess.run(["curl", "-sSLf", "--retry", "3", "--connect-timeout", "20",
                        "--speed-limit", "1000", "--speed-time", "60", "--max-time", "900",
                        "-o", partial, url], check=True)
        Path(partial).rename(target)
    except BaseException:
        Path(partial).unlink(missing_ok=True)
        raise
    return target


def ordered(name, directory, pin):
    """scripts/unpack's directory order, for one package."""
    package = directory / "patches"
    project = CORELEC / f"projects/{PROJECT}/patches/{name}"
    device = CORELEC / f"projects/{PROJECT}/devices"
    upstream = [package, package / PATCH_ARCH, package / pin, package / pin / PATCH_ARCH,
                project, project / PATCH_ARCH, project / pin]
    upstream += sorted(device.glob(f"*/patches/{name}"))
    result = [(path, False) for directory in upstream if directory.is_dir()
              for path in sorted(directory.glob("*.patch"))]
    ours = [(f"{path.parent.parent.name.split('-', 1)[0]}_{path.name}", path)
            for path in (REPO / "patches-yacer").glob(f"*/{name}/*.patch")]
    result.extend((path, True) for _, path in sorted(ours))
    return result


def unpack(archive, destination):
    if archive.name.endswith((".tar.gz", ".tgz", ".tar.xz", ".tar.bz2", ".tar.zst")):
        with tarfile.open(archive) as handle:
            members = []
            for member in handle:
                member.name = member.name.partition("/")[2]
                if member.name:
                    members.append(member)
            handle.extractall(destination, members=members, filter="data")
        return True
    return False


def check(name, download, keep):
    directory = package_dir(name)
    if directory is None:
        return name, "no package.mk in the tree", 0
    pin = declared(directory / "package.mk", "PKG_VERSION")
    archive = archive_for(name, directory, pin, download)
    if archive is None:
        return name, f"no cached source for pin {pin or '?'}", 0
    work = Path(tempfile.mkdtemp(prefix=f"yacer-{name}-"))
    try:
        if not unpack(archive, work):
            return name, f"cannot unpack {archive.name}", 0
        applied = 0
        for path, yacer in ordered(name, directory, pin):
            text = path.read_text(errors="replace")
            if re.search(r"^(GIT binary patch|rename from|rename to)", text, re.MULTILINE):
                command = ["git", "apply", "-p1", "--whitespace=nowarn"]
            else:
                command = ["patch", "-p1", "--batch", "--forward", "--no-backup-if-mismatch"]
                if yacer:
                    command.append("--fuzz=0")
            result = subprocess.run(command, input=text, text=True, cwd=work,
                                    capture_output=True, timeout=300)
            if result.returncode:
                detail = (result.stdout + result.stderr).strip().splitlines()
                return name, f"{path.name}\n      " + "\n      ".join(detail[:6]), applied
            applied += 1
        return name, None, applied
    finally:
        if not keep:
            shutil.rmtree(work, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--offline", action="store_true", help="never fetch a missing source")
    parser.add_argument("--keep", action="store_true", help="leave the unpacked trees behind")
    parser.add_argument("packages", nargs="*", help="only these packages")
    args = parser.parse_args()
    sys.dont_write_bytecode = True

    wanted = args.packages or packages()
    bad = 0
    for name in wanted:
        label, problem, applied = check(name, not args.offline, args.keep)
        if problem is None:
            print(f"  ok      {label:<20} {applied} patches")
        else:
            print(f"  FAILED  {label:<20} after {applied}: {problem}")
            bad += 1
    print(f"{len(wanted) - bad}/{len(wanted)} package stacks apply")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
