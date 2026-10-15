#!/usr/bin/env python3
"""Carry our patch stacks from one CoreELEC commit to another.

When upstream moves a package under our patches, re-applying them with fuzz
guesses; this replays them with git's 3-way merge instead, so most of them land
on their own and the rest stop as ordinary merge conflicts:

  prepare  For each package (default: every one whose stack does not apply to
           NEW), build a git repository in WORK/<package>: the package's source
           at OLD with upstream's own patches, one commit per patch of ours,
           and the source at NEW with upstream's patches as a second base. Then
           rebase our commits onto the new base. A package either finishes or
           stops at its first conflict.
  status   Which packages are done and which stop where, with the files in
           conflict.
  export   For every package whose rebase is finished, write each patch back
           into patches-yacer/ - its own header, the diff regenerated - and
           prove the result: applied to a fresh NEW base at zero fuzz, the
           patches must give exactly the rebased tree.

Resolving a stop is ordinary git, in WORK/<package>: edit the files, `git add`
them, `GIT_EDITOR=true git rebase --continue`. A patch whose change upstream
already has becomes empty and git drops it; export reports it, and removes its
file only with --drop-empty.

OLD and NEW are CoreELEC checkouts (the trees whose package.mk files name the
sources); our patches are read from this branch. Sources are fetched into each
tree's sources/ the way check-patch-stack.py does.
"""

import argparse
import importlib.util
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

REPO = Path(__file__).resolve().parents[1]
sys.dont_write_bytecode = True
sys.path.insert(0, str(REPO / "tests"))
_spec = importlib.util.spec_from_file_location("check_patch_stack", REPO / "tests/check-patch-stack.py")
stack = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(stack)

# Where a patch's description ends: its first file header.
HEADER_END = re.compile(rb"^(diff --git |--- \S.*\n\+\+\+ )", re.MULTILINE)
GIT_ENV = {**os.environ, "GIT_AUTHOR_NAME": "yacer", "GIT_AUTHOR_EMAIL": "yacer@localhost",
           "GIT_COMMITTER_NAME": "yacer", "GIT_COMMITTER_EMAIL": "yacer@localhost",
           "GIT_EDITOR": "true"}


def git(repo, *args, check=True):
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, env=GIT_ENV)
    if check and result.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout.decode(errors="replace").strip()


def git_bytes(repo, *args):
    """A command's output exactly as git wrote it - diffs are never trimmed."""
    result = subprocess.run(["git", "-C", str(repo), *args], capture_output=True, env=GIT_ENV)
    if result.returncode:
        raise RuntimeError(f"git {' '.join(args)}: {result.stderr.decode(errors='replace').strip()}")
    return result.stdout


def apply_one(path, work, yacer):
    """Apply one patch the way check-patch-stack.py (and scripts/unpack) does."""
    text = path.read_text(errors="replace")
    if re.search(r"^(GIT binary patch|rename from|rename to)", text, re.MULTILINE):
        command = ["git", "apply", "-p1", "--whitespace=nowarn"]
    else:
        command = ["patch", "-p1", "--batch", "--forward", "--no-backup-if-mismatch"]
        if yacer:
            command.append("--fuzz=0")
    result = subprocess.run(command, input=text, text=True, cwd=work, capture_output=True,
                            timeout=300)
    if result.returncode:
        detail = (result.stdout + result.stderr).strip().splitlines()
        raise RuntimeError(f"{path.name} does not apply:\n  " + "\n  ".join(detail[:8]))


def source_tree(name, tree, dest, download):
    """Unpack NAME's pinned source from TREE into DEST with upstream's patches.
    Returns the list of our patches, in order, as (flattened name, path)."""
    directory = stack.package_dir(name, tree)
    if directory is None:
        raise RuntimeError(f"{name}: no package.mk in {tree}")
    pin = stack.declared(directory / "package.mk", "PKG_VERSION")
    archive = stack.archive_for(name, directory, pin, download, tree)
    if archive is None:
        raise RuntimeError(f"{name}: no source for {pin or '?'} in {tree}/sources")
    dest.mkdir(parents=True)
    if not stack.unpack(archive, dest):
        raise RuntimeError(f"{name}: cannot unpack {archive.name}")
    ours = []
    for path, yacer in stack.ordered(name, directory, pin, tree):
        if yacer:
            ours.append(path)
        else:
            apply_one(path, dest, False)
    return pin, ours


def snapshot(repo, src, message, parent=None):
    """Commit the whole of SRC as one commit (on PARENT, or as a root)."""
    index = repo / ".git" / "yacer-index"
    env = {**GIT_ENV, "GIT_INDEX_FILE": str(index)}
    index.unlink(missing_ok=True)
    subprocess.run(["git", "-C", str(repo), "--work-tree", str(src), "add", "-A", "-f", "."],
                   env=env, check=True, capture_output=True)
    tree = subprocess.run(["git", "-C", str(repo), "write-tree"], env=env, check=True,
                          capture_output=True, text=True).stdout.strip()
    index.unlink(missing_ok=True)
    args = ["commit-tree", tree, "-m", message] + (["-p", parent] if parent else [])
    return git(repo, *args)


def prepare(args):
    work = Path(args.work).resolve()
    work.mkdir(parents=True, exist_ok=True)
    old, new = Path(args.old).resolve(), Path(args.new).resolve()
    names = args.packages or failing(new, not args.offline)
    if not names:
        print("every stack applies to NEW - nothing to re-anchor")
    failed = 0
    for name in names:
        repo = work / name
        if repo.exists():
            shutil.rmtree(repo)
        repo.mkdir()
        try:
            prepare_one(name, repo, old, new, not args.offline)
        except (RuntimeError, OSError, subprocess.SubprocessError) as error:
            failed += 1
            print(f"{name}: cannot prepare - {error}")
            (repo / ".git" / "yacer-reanchor.json").write_text(json.dumps(
                {"package": name, "old": "?", "new": "?", "patches": 0, "error": str(error)}))
            continue
        report(repo)
    return 1 if failed else 0


def prepare_one(name, repo, old, new, download):
    git(repo, "init", "-q", "-b", "series")
    # Commits are made here by whoever resolves a stop, with no identity of
    # their own; and blobs must be stored exactly as the files are. Diffs stay
    # text: a patch written as a git binary delta cannot be read or reviewed,
    # and applies only to the very file it was made from.
    git(repo, "config", "core.autocrlf", "false")
    git(repo, "config", "user.name", "yacer")
    git(repo, "config", "user.email", "yacer@localhost")
    (repo / ".git" / "info").mkdir(exist_ok=True)
    (repo / ".git" / "info" / "attributes").write_text("* -text -filter -ident\n")
    scratch = Path(tempfile.mkdtemp(prefix=f"reanchor-{name}-"))
    try:
        old_pin, ours = source_tree(name, old, scratch / "old", download)
        base_old = snapshot(repo, scratch / "old", f"{name} {old_pin} with upstream's patches")
        head = base_old
        for path in ours:
            apply_one(path, scratch / "old", True)
            rel = path.relative_to(REPO)
            label = f"{path.parent.parent.name.split('-', 1)[0]}_{path.name}"
            head = snapshot(repo, scratch / "old", f"{label}\n\nyacer-patch: {rel}", head)
        new_pin, _ = source_tree(name, new, scratch / "new", download)
        base_new = snapshot(repo, scratch / "new", f"{name} {new_pin} with upstream's patches")
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    git(repo, "update-ref", "refs/heads/series", head)
    git(repo, "tag", "-f", "base-old", base_old)
    git(repo, "tag", "-f", "base-new", base_new)
    git(repo, "tag", "-f", "series-old", head)
    git(repo, "reset", "-q", "--hard", "series")
    (repo / ".git" / "yacer-reanchor.json").write_text(json.dumps(
        {"package": name, "old": old_pin, "new": new_pin, "patches": len(ours)}))
    subprocess.run(["git", "-C", str(repo), "rebase", "--empty=drop", "--onto",
                    "base-new", "base-old", "series"], capture_output=True, env=GIT_ENV)
    print(f"{name}: {old_pin[:12]} -> {new_pin[:12]}, {len(ours)} patches")


def failing(tree, download):
    """The packages whose stack does not apply to TREE."""
    names = []
    for name in stack.packages():
        dest = Path(tempfile.mkdtemp(prefix=f"reanchor-check-{name}-"))
        try:
            directory = stack.package_dir(name, tree)
            if directory is None:
                raise RuntimeError("no package.mk")
            pin = stack.declared(directory / "package.mk", "PKG_VERSION")
            archive = stack.archive_for(name, directory, pin, download, tree)
            if archive is None or not stack.unpack(archive, dest):
                raise RuntimeError(f"no usable source for {pin or '?'}")
            for path, yacer in stack.ordered(name, directory, pin, tree):
                apply_one(path, dest, yacer)
        except Exception as error:      # anything at all: that stack needs a look
            print(f"{name}: does not apply to NEW ({str(error).splitlines()[0]})")
            names.append(name)
        finally:
            shutil.rmtree(dest, ignore_errors=True)
    return names


def load_meta(repo):
    try:
        return json.loads((repo / ".git" / "yacer-reanchor.json").read_text())
    except (OSError, ValueError):
        return {"error": "no record of a prepare here"}


def in_rebase(repo):
    return (repo / ".git" / "rebase-merge").is_dir() or (repo / ".git" / "rebase-apply").is_dir()


def report(repo):
    if in_rebase(repo):
        stopped = (repo / ".git" / "rebase-merge" / "stopped-sha")
        sha = stopped.read_text().strip() if stopped.exists() else git(repo, "rev-parse", "REBASE_HEAD", check=False)
        subject = git(repo, "log", "-1", "--format=%s", sha, check=False) if sha else "?"
        conflicts = git(repo, "diff", "--name-only", "--diff-filter=U").splitlines()
        done = int(git(repo, "rev-list", "--count", "base-new..HEAD") or 0)
        print(f"  STOPPED at {subject} ({done} replayed before it)")
        for path in conflicts:
            print(f"    conflict: {path}")
        return False
    total = int(git(repo, "rev-list", "--count", "base-old..series-old"))
    landed = int(git(repo, "rev-list", "--count", "base-new..series"))
    print(f"  done: {landed} of {total} patches replayed"
          + (f", {total - landed} became empty" if landed < total else ""))
    return True


def status(args):
    work = Path(args.work).resolve()
    ok = True
    for repo in sorted(p for p in work.iterdir() if (p / ".git").is_dir()):
        meta = load_meta(repo)
        if meta.get("error"):
            print(f"{repo.name}: not prepared - {meta['error']}")
            ok = False
            continue
        print(f"{repo.name}: {meta['old'][:12]} -> {meta['new'][:12]}")
        ok = report(repo) and ok
    return 0 if ok else 1


def header_of(data, path):
    match = HEADER_END.search(data)
    if not match:
        raise RuntimeError(f"{path}: cannot find where its description ends")
    return data[:match.start()]


def export(args):
    work = Path(args.work).resolve()
    new = Path(args.new).resolve()
    bad = 0
    for repo in sorted(p for p in work.iterdir() if (p / ".git").is_dir()):
        name = repo.name
        if load_meta(repo).get("error"):
            print(f"{name}: not prepared - nothing to export")
            bad += 1
            continue
        if in_rebase(repo):
            print(f"{name}: rebase not finished - resolve it first")
            bad += 1
            continue
        landed = {}
        for sha in git(repo, "rev-list", "--reverse", "base-new..series").splitlines():
            body = git(repo, "log", "-1", "--format=%B", sha)
            match = re.search(r"^yacer-patch: (.+)$", body, re.MULTILINE)
            if not match:
                raise RuntimeError(f"{name}: commit {sha[:10]} has no yacer-patch trailer")
            landed[match[1]] = sha
        originals = {}
        for sha in git(repo, "rev-list", "--reverse", "base-old..series-old").splitlines():
            originals[re.search(r"^yacer-patch: (.+)$", git(repo, "log", "-1", "--format=%B", sha),
                                re.MULTILINE)[1]] = sha
        rewritten = kept = 0
        # Replay the stack on the new base in a worktree. A patch keeps its file
        # when that file, applied at zero fuzz, gives exactly the rebased commit's
        # tree - line numbers and hunk shapes may differ, the result may not.
        # Otherwise the commit's own diff is written. Either way every step ends
        # on the rebased commit's tree, so the next patch is tried on the right one.
        trial = Path(tempfile.mkdtemp(prefix=f"reanchor-export-{name}-")) / "t"
        git(repo, "worktree", "add", "-q", "--detach", str(trial), "base-new")
        try:
            for rel, old_sha in originals.items():
                path = REPO / rel
                if rel not in landed:
                    if args.drop_empty:
                        path.unlink()
                        print(f"{name}: {rel} became empty (upstream has it) - removed")
                    else:
                        print(f"{name}: {rel} became empty (upstream has it) - kept; --drop-empty removes it")
                    continue
                sha = landed[rel]
                same = False
                try:
                    apply_one(path, trial, True)
                    git(trial, "add", "-A", ".")
                    same = subprocess.run(["git", "-C", str(trial), "diff", "--cached", "--quiet", sha],
                                          env=GIT_ENV).returncode == 0
                except RuntimeError:
                    pass
                if same:
                    kept += 1
                else:
                    diff = git_bytes(repo, "diff", "--no-color", "--no-renames", "-U3",
                                     f"{sha}~1", sha)
                    path.write_bytes(header_of(path.read_bytes(), rel) + diff)
                    rewritten += 1
                git(trial, "reset", "-q", "--hard", sha)
                git(trial, "clean", "-q", "-fdx")
        finally:
            git(repo, "worktree", "remove", "--force", str(trial), check=False)
            shutil.rmtree(trial.parent, ignore_errors=True)
        # The proof: a fresh NEW base plus the files as written must be the rebased tree.
        scratch = Path(tempfile.mkdtemp(prefix=f"reanchor-verify-{name}-"))
        try:
            _, ours = source_tree(name, new, scratch / "t", not args.offline)
            for path in ours:
                apply_one(path, scratch / "t", True)
            tree = snapshot(repo, scratch / "t", "verify")
            same = git(repo, "rev-parse", f"{tree}^{{tree}}") == git(repo, "rev-parse", "series^{tree}")
        except RuntimeError as error:
            same = False
            print(f"{name}: {error}")
        finally:
            shutil.rmtree(scratch, ignore_errors=True)
        print(f"{name}: {rewritten} patches rewritten, {kept} unchanged, "
              + ("verified at zero fuzz" if same else "VERIFY FAILED - the files do not give the rebased tree"))
        bad += 0 if same else 1
    return 1 if bad else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    p = sub.add_parser("prepare")
    p.add_argument("--old", required=True, help="CoreELEC checkout the patches apply to now")
    p.add_argument("--new", required=True, help="CoreELEC checkout to carry them to")
    p.add_argument("--work", required=True, help="where the per-package repositories go")
    p.add_argument("--offline", action="store_true")
    p.add_argument("packages", nargs="*")
    s = sub.add_parser("status")
    s.add_argument("--work", required=True)
    e = sub.add_parser("export")
    e.add_argument("--new", required=True)
    e.add_argument("--work", required=True)
    e.add_argument("--offline", action="store_true")
    e.add_argument("--drop-empty", action="store_true")
    args = parser.parse_args()
    try:
        return {"prepare": prepare, "status": status, "export": export}[args.command](args)
    except RuntimeError as error:
        print(f"yacer-reanchor: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
