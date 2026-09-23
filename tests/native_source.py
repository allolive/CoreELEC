"""Stage only requested files from the pinned Amlogic Kodi source and patch stack.

The default is the repository's Amlogic-ce/aarch64 package and no-display-server
patch order (scripts/unpack), followed by yacer's flattened group/patch names
(scripts/yacer-patches.sh). No configured build tree or docs overlay is needed.
An explicit overlay replaces matching files after staging the same dependencies.
"""

import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tarfile
import tempfile


REPO = Path(__file__).resolve().parents[1]
WORKSPACE = REPO.parent
CORELEC = WORKSPACE / "CoreELEC"
PACKAGE = CORELEC / "projects/Amlogic-ce/packages/mediacenter/kodi"


def ordered_patches(pin):
    package = PACKAGE / "patches"
    project = CORELEC / "projects/Amlogic-ce/patches/kodi"
    # Optional cec-framework directories are empty in this package today.
    # Wayland/X11 patches are not part of the no-display-server AM9 build.
    directories = (package, package / "aarch64", package / "cec-framework",
                   package / pin, package / pin / "aarch64", project,
                   project / "aarch64", project / "cec-framework", project / pin)
    result = [(path, False) for directory in directories
              for path in sorted(directory.glob("*.patch"))]
    patches = (REPO / "patches-yacer").glob("*/kodi/*.patch")
    flattened = [(f"{path.parent.parent.name.split('-', 1)[0]}_{path.name}", path)
                 for path in patches]
    if len({name for name, _ in flattened}) != len(flattened):
        raise RuntimeError("duplicate flattened Kodi patch name")
    result.extend((path, True) for _, path in sorted(flattened))
    return result


# Lines git puts before the ---/+++ pair. They belong to the file block that
# follows, so a block starts at the first of them rather than at the ---.
PREAMBLE = ("diff --git ", "index ", "old mode ", "new mode ", "new file mode ",
            "deleted file mode ", "similarity index ", "dissimilarity index ",
            "rename from ", "rename to ", "copy from ", "copy to ", "GIT binary patch")


def diff_path(line):
    path = line[4:].split("\t")[0].strip()
    if path == "/dev/null":
        return None
    # These are applied with -p1, which strips one leading component whatever
    # it is called. Stripping only "a/" and "b/" left a diff taken as
    # "kodi.orig/..." unmatched, so a patch touching a staged file filtered to
    # nothing and was skipped without a word.
    head, separator, rest = path.partition("/")
    return rest if separator else path


def file_blocks(text):
    """Yield (old, new, block) per file, for git diffs and plain diff -u alike.

    Splitting on "diff --git" alone drops every file of a patch regenerated
    with plain diff, which has no such line. The ---/+++ pair is the one
    header both shapes carry.
    """
    lines = text.splitlines(keepends=True)
    heads = [index for index in range(len(lines) - 1)
             if lines[index].startswith("--- ") and lines[index + 1].startswith("+++ ")]
    for position, index in enumerate(heads):
        start = index
        while start and lines[start - 1].startswith(PREAMBLE):
            start -= 1
        end = len(lines) if position + 1 == len(heads) else heads[position + 1]
        while end > index + 2 and lines[end - 1].startswith(PREAMBLE):
            end -= 1
        yield diff_path(lines[index]), diff_path(lines[index + 1]), "".join(lines[start:end])


def kodi_patch_roots(pin):
    package = PACKAGE / "patches"
    project = CORELEC / "projects/Amlogic-ce/patches/kodi"
    devices = CORELEC / "projects/Amlogic-ce/devices"
    # Only Kodi's own directories: the devices tree also holds patches for
    # other packages, which are none of this harness's business.
    return [package, project] + sorted(devices.glob("*/patches/kodi"))


def check_patches_accounted(pin, ordered):
    """Refuse to run if a patch on disk is not in the order we apply.

    ordered_patches() reproduces the directory list in scripts/unpack by hand.
    If upstream adds a directory - a device-specific one, say - the tests would
    quietly certify a tree the build does not produce. Compare against what is
    actually there instead of trusting the copy to stay in step.
    """
    known = {path.resolve() for path, _ in ordered}
    stray = []
    for root in kodi_patch_roots(pin):
        if not root.is_dir():
            continue
        for path in root.rglob("*.patch"):
            if path.resolve() not in known:
                stray.append(path)
    if stray:
        listed = "\n".join(f"  {path}" for path in sorted(stray))
        raise RuntimeError(
            "these Kodi patches are on disk but not in the order the tests "
            f"apply, so the build would use them and the tests would not:\n{listed}")


def filtered_patch(text, required):
    result = []
    for old, new, block in file_blocks(text):
        if old in required or new in required:
            if old and new and old != new:
                raise RuntimeError("requested source was renamed; stage both paths explicitly")
            result.append(block)
    return "".join(result)


def prepare_source(destination, required, overlay=None):
    destination = Path(destination)
    required = set(required)
    if not required or any(Path(path).is_absolute() or ".." in Path(path).parts
                           for path in required):
        raise ValueError("expected nonempty relative source paths")
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise ValueError("source staging directory must be empty")
    match = re.search(r'^PKG_VERSION="([^"]+)"', (PACKAGE / "package.mk").read_text(), re.MULTILINE)
    if not match:
        raise RuntimeError("could not find the Amlogic Kodi package pin")
    pin = match[1]
    archive_path = CORELEC / f"sources/kodi/kodi-{pin}.tar.gz"
    with tarfile.open(archive_path, "r:gz") as archive:
        for member in archive:
            relative = member.name.partition("/")[2]
            if relative in required and member.isfile():
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(archive.extractfile(member).read())

    manifest = {"pin": pin, "archive": str(archive_path.relative_to(WORKSPACE)),
                "patches": [], "overrides": [], "files": {}}
    for path, yacer in ordered_patches(pin):
        original = path.read_text()
        # A patch whose shape the splitter does not recognise filters to nothing
        # and would be skipped in silence, leaving the tests to certify a tree
        # that is missing it. Refuse the run instead.
        if not any(file_blocks(original)):
            raise RuntimeError(f"{path}: no file header recognised")
        patch = filtered_patch(original, required)
        if not patch:
            continue
        command = ["patch", "-p1", "--batch", "--forward", "--no-backup-if-mismatch"]
        if yacer:
            command.append("--fuzz=0")
        result = subprocess.run(command, input=patch, text=True, cwd=destination,
                                capture_output=True, timeout=30)
        if result.returncode:
            raise RuntimeError(f"{path}:\n{result.stdout}{result.stderr}")
        manifest["patches"].append({"path": str(path.relative_to(WORKSPACE)),
                                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()})
    if overlay is not None:
        overlay = Path(overlay).resolve()
        if not overlay.is_dir():
            raise ValueError(f"source overlay directory does not exist: {overlay}")
        for relative in sorted(required):
            original = overlay / relative
            if original.is_file():
                target = destination / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(original.read_bytes())
                manifest["overrides"].append(str(original))
        if not manifest["overrides"]:
            raise ValueError(f"source overlay contains none of the requested files: {overlay}")
    missing = sorted(path for path in required if not (destination / path).is_file())
    if missing:
        raise RuntimeError(f"missing patched production files: {missing}")
    manifest["files"] = {path: hashlib.sha256((destination / path).read_bytes()).hexdigest()
                         for path in sorted(required)}
    identity = hashlib.sha256(json.dumps(manifest, sort_keys=True).encode()).hexdigest()
    (destination / "native-source.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Production source: Kodi {pin}, {len(manifest['patches'])} ordered patches, "
          f"{len(required)} files, {len(manifest['overrides'])} overrides; sha256={identity}", flush=True)
    return manifest


FFMPEG = CORELEC / "packages/multimedia/ffmpeg"
LIBFMT = CORELEC / "packages/devel/libfmt"
# ffmpeg generates these at configure time, so a release tarball has neither.
GENERATED = {
    "libavutil/avconfig.h": """/* staged for host-side tests; ffmpeg generates this at configure time */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 1
#endif
""",
    "libavutil/ffversion.h": """/* staged for host-side tests; ffmpeg generates this at configure time */
#ifndef AVUTIL_FFVERSION_H
#define AVUTIL_FFVERSION_H
#define FFMPEG_VERSION "{version}"
#endif
""",
}


def pinned_archive(package, suffix):
    """A package's pinned source, verified against the PKG_SHA256 it declares."""
    mk = (package / "package.mk").read_text()
    version = re.search(r'^PKG_VERSION="([^"]+)"', mk, re.MULTILINE)[1]
    want = re.search(r'^PKG_SHA256="([^"]*)"', mk, re.MULTILINE)
    archive = CORELEC / f"sources/{package.name}/{package.name}-{version}{suffix}"
    if not archive.is_file():
        raise RuntimeError(f"missing {archive}; a build fetches it, or fetch it by hand")
    if want and want[1] and file_sha256(archive) != want[1]:
        raise RuntimeError(f"{archive} does not match the PKG_SHA256 in {package}/package.mk")
    return archive, version


# ffmpeg's build generates config.h, and crc.c reaches for an internal
# thread.h. Both are small enough to write out for the host, which is what
# lets the real CRC be compiled rather than imitated.
FFMPEG_CONFIG = """/* staged for host-side tests; ffmpeg's build generates this */
#define CONFIG_HARDCODED_TABLES 0
#define CONFIG_SMALL 0
#define ARCH_X86 0
#define ARCH_AARCH64 0
"""
FFMPEG_THREAD = """/* staged for host-side tests; the internal header is not shipped */
#ifndef AVUTIL_THREAD_H
#define AVUTIL_THREAD_H
#include <pthread.h>
typedef pthread_once_t AVOnce;
#define AV_ONCE_INIT PTHREAD_ONCE_INIT
static inline int ff_thread_once(AVOnce *control, void (*routine)(void))
{ return pthread_once(control, routine); }
#endif
"""
# ffmpeg sources a suite may compile in, rather than stand in for. A checksum
# is worth nothing if the test computes it with an imitation.
FFMPEG_SOURCES = ("libavutil/crc.c",)


def staged_sources(destination):
    """Stage the few ffmpeg sources a suite compiles rather than fakes."""
    destination = Path(destination)
    if (destination / FFMPEG_SOURCES[0]).is_file():
        return destination
    archive, version = pinned_archive(FFMPEG, ".tar.xz")
    root = f"ffmpeg-{version}/"
    wanted = {root + name for name in FFMPEG_SOURCES}
    with tarfile.open(archive) as tar:
        for member in tar:
            if member.isfile() and member.name in wanted:
                target = destination / member.name[len(root):]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
    (destination / "config.h").write_text(FFMPEG_CONFIG)
    (destination / "libavutil/thread.h").write_text(FFMPEG_THREAD)
    return destination


def staged_headers(destination):
    """Stage the declaration headers a suite may need, from pinned sources.

    Kodi's utility translation units include FFmpeg and fmt. Taking those from
    the host tests against whatever happens to be installed rather than what
    the box runs, and a CI runner has neither. Both packages declare a
    PKG_SHA256, so the archives are verified rather than trusted.
    """
    destination = Path(destination)
    if (destination / "libavutil/intreadwrite.h").is_file() and (destination / "fmt").is_dir():
        return destination
    destination.mkdir(parents=True, exist_ok=True)

    archive, version = pinned_archive(FFMPEG, ".tar.xz")
    root = f"ffmpeg-{version}/"
    with tarfile.open(archive) as tar:
        for member in tar:
            name = member.name
            if not (member.isfile() and name.startswith(root) and name.endswith(".h")):
                continue
            relative = name[len(root):]
            library, _, leaf = relative.partition("/")
            if not leaf or "/" in leaf or not library.startswith(("libav", "libsw")):
                continue
            target = destination / library / leaf
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(tar.extractfile(member).read())
    for relative, body in GENERATED.items():
        (destination / relative).write_text(body.format(version=version))

    archive, version = pinned_archive(LIBFMT, ".tar.gz")
    root = f"fmt-{version}/include/"
    with tarfile.open(archive) as tar:
        for member in tar:
            name = member.name
            if member.isfile() and name.startswith(root) and name.endswith(".h"):
                target = destination / name[len(root):]
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(tar.extractfile(member).read())
    return destination


def kodi_pin():
    match = re.search(r'^PKG_VERSION="([^"]+)"', (PACKAGE / "package.mk").read_text(), re.MULTILINE)
    if not match:
        raise RuntimeError("could not find the Amlogic Kodi package pin")
    return match[1]


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def kodi_archive(pin, download=True):
    """The pinned Kodi archive, fetched into the tree's own source cache.

    The package declares no PKG_SHA256, so the build trusts the archive on
    first download and records what it got in a .sha256 beside it. Do the
    same, and check against that record before reusing a cached copy: a
    truncated body that still arrived with a 200 would otherwise be compiled
    for the life of the cache entry.
    """
    archive = CORELEC / f"sources/kodi/kodi-{pin}.tar.gz"
    record = Path(f"{archive}.sha256")
    if archive.is_file():
        if record.is_file():
            want = record.read_text().split()[0]
            got = file_sha256(archive)
            if got != want:
                raise RuntimeError(
                    f"{archive} does not match {record}:\n"
                    f"  recorded {want}\n  found    {got}\n"
                    "Delete the archive to fetch it again.")
        return archive
    if not download:
        raise RuntimeError(f"missing {archive}")
    archive.parent.mkdir(parents=True, exist_ok=True)
    url = f"https://github.com/CoreELEC/xbmc/archive/{pin}.tar.gz"
    # A fixed .part name in a directory the product build also writes would
    # let two runs splice one file together.
    handle, partial = tempfile.mkstemp(dir=archive.parent, prefix=".kodi-", suffix=".part")
    os.close(handle)
    partial = Path(partial)
    try:
        # --connect-timeout only bounds the handshake; --speed-limit with
        # --speed-time is what gives up on a transfer that connects and then
        # stalls, which would otherwise hold the run until its whole timeout.
        subprocess.run(["curl", "-sSLf", "--retry", "3", "--connect-timeout", "20",
                        "--speed-limit", "1000", "--speed-time", "60", "--max-time", "900",
                        "-o", str(partial), url], check=True)
        digest = file_sha256(partial)
        partial.rename(archive)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise
    record.write_text(digest)
    Path(f"{archive}.url").write_text(url)
    return archive


def prepare_tree(destination, download=True):
    """Unpack the pinned Kodi and apply every ordered patch to the whole tree.

    This is scripts/unpack's job done outside a build: the same archive, the
    same patch order, the same zero fuzz for ours. A patch that no longer
    applies stops it here rather than hours into a build.
    """
    destination = Path(destination)
    destination.mkdir(parents=True, exist_ok=True)
    if any(destination.iterdir()):
        raise ValueError("source tree directory must be empty")
    pin = kodi_pin()
    archive_path = kodi_archive(pin, download)
    with tarfile.open(archive_path, "r:gz") as archive:
        members = []
        for member in archive:
            member.name = member.name.partition("/")[2]
            if member.name:
                members.append(member)
        archive.extractall(destination, members=members, filter="data")

    ordered = ordered_patches(pin)
    check_patches_accounted(pin, ordered)
    applied = []
    for path, yacer in ordered:
        text = path.read_text()
        # scripts/unpack hands these to git apply, which GNU patch cannot do.
        if re.search(r"^(GIT binary patch|rename from|rename to)", text, re.MULTILINE):
            command = ["git", "apply", "-p1", "--whitespace=nowarn"]
        else:
            command = ["patch", "-p1", "--batch", "--forward", "--no-backup-if-mismatch"]
            if yacer:
                command.append("--fuzz=0")
        result = subprocess.run(command, input=text, text=True, cwd=destination,
                                capture_output=True, timeout=120)
        if result.returncode:
            raise RuntimeError(f"{path.name} did not apply:\n{result.stdout}{result.stderr}")
        # Never use -s here: it hides the fuzz line the zero-fuzz rule needs.
        fuzz = [line for line in result.stdout.splitlines() if "with fuzz" in line]
        applied.append((path, yacer, fuzz))
    manifest = {
        "pin": pin,
        "archive_sha256": file_sha256(archive_path),
        "patches": [{"path": str(path.relative_to(WORKSPACE)),
                     "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                     "yacer": yacer} for path, yacer, _ in applied],
    }
    # Suites that compile Kodi translation units need these; staging them
    # beside the tree keeps the whole compile hermetic.
    staged_headers(destination / ".yacer-include")
    staged_sources(destination / ".yacer-src")
    # Written last: its absence means the tree is half-built.
    (destination / "yacer-tree.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Applied {len(applied)} ordered Kodi patches to {destination} (pin {pin})", flush=True)
    return applied
