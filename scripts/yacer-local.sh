#!/usr/bin/env bash
#
# Assemble the yacer branch into this checkout for a local build, and take it
# back out again afterwards.
#
# CI builds a throwaway tree: check out the mirror, copy our files over it, go.
# Locally there is one clone and it has to keep working as a checkout of the
# mirror in between, so the same assembly has to be reversible.
#
#   yacer-local.sh apply    lay the yacer branch over this checkout
#   yacer-local.sh revert    take it back out
#   yacer-local.sh status    say which state the checkout is in
#
# Run from the root of the CoreELEC clone. The yacer branch is read out of this
# same repository, so no second working copy is needed.
set -euo pipefail

BRANCH=${YACER_BRANCH:-yacer}
STATE=.yacer-local-state

die() { echo "yacer-local: $*" >&2; exit 1; }

[ -d .git ] || die "run this from the root of the CoreELEC clone"
git rev-parse --verify --quiet "$BRANCH" >/dev/null || die "no $BRANCH branch in this clone"

cmd=${1:-status}

case "$cmd" in
status)
  if [ -f "$STATE" ]; then
    echo "applied: $(grep -c '^file ' "$STATE" || true) overlay file(s), $(grep -c '^patch ' "$STATE" || true) tree-patch(es)"
    echo "from $BRANCH @ $(sed -n 's/^commit //p' "$STATE")"
  else
    echo "clean: this is a plain checkout of the mirror"
  fi
  ;;

apply)
  [ -f "$STATE" ] && die "already applied - run 'revert' first"
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  git archive "$BRANCH" | tar -x -C "$tmp"
  commit=$(git rev-parse "$BRANCH")

  # The tree-patches are written against the mirror, so the files they edit have
  # to look the way the mirror has them. A clone sitting on its own commits - or
  # with local edits to those files - is not the tree they were made for, and
  # applying anyway would either fail here or, worse, apply somewhere it fits.
  mirror=${YACER_MIRROR:-origin/coreelec-22}
  if ! git rev-parse --verify --quiet "$mirror" >/dev/null; then
    die "no $mirror in this clone - fetch it, or set YACER_MIRROR"
  fi
  for p in "$tmp"/tree-patches/*.patch; do
    [ -e "$p" ] || continue
    while read -r f; do
      [ -n "$f" ] || continue
      if ! git diff --quiet -- "$f" 2>/dev/null; then
        die "$f is modified locally and $(basename "$p") edits it - commit or revert it first"
      fi
      if ! git diff --quiet "$mirror" -- "$f" 2>/dev/null; then
        die "$f differs from $mirror and $(basename "$p") edits it.
     This clone is not at the mirror state the patches were written against.
     Check out the mirror to test the branch, or set YACER_MIRROR to whatever
     this clone is actually tracking."
      fi
    done < <(sed -n 's|^--- a/||p' "$p")
  done

  # A patch of ours that is also sitting loose in the package's patch directory
  # would be applied twice, and the second time would fail.
  while IFS= read -r -d '' f; do
    rel=${f#"$tmp"/overlay/}
    case "$rel" in */patches-yacer/*/*.patch) ;; *) continue;; esac
    loose=${rel/\/patches-yacer\//\/patches\/}
    [ -e "$loose" ] && die "$loose is also in the tree - it would be applied twice"
  done < <(find "$tmp/overlay" -type f -print0 2>/dev/null)

  : > "$STATE"
  echo "commit $commit" >> "$STATE"

  while IFS= read -r -d '' f; do
    rel=${f#"$tmp"/overlay/}
    [ -e "$rel" ] && die "overlay would overwrite $rel - use a tree-patch instead"
    mkdir -p "$(dirname "$rel")"
    cp -p "$f" "$rel"
    echo "file $rel" >> "$STATE"
    echo "  added   $rel"
  done < <(find "$tmp/overlay" -type f -print0 2>/dev/null)

  # Roll back what landed if a later patch fails, so a failed apply never leaves
  # a half-assembled tree behind.
  rollback() {
    echo "  rolling back" >&2
    tac "$STATE" | sed -n 's/^patch //p' | while read -r n; do
      git apply -R --whitespace=nowarn "$tmp/tree-patches/$n" 2>/dev/null || true
    done
    sed -n 's/^file //p' "$STATE" | while read -r rel; do rm -f "$rel"; done
    rm -f "$STATE"
  }
  for p in "$tmp"/tree-patches/*.patch; do
    [ -e "$p" ] || continue
    if ! git apply --whitespace=nowarn "$p"; then
      rollback
      die "$(basename "$p") does not apply - rebase it"
    fi
    echo "patch $(basename "$p")" >> "$STATE"
    echo "  patched $(basename "$p")"
  done

  if ! grep -q '^file \|^patch ' "$STATE"; then
    rm -f "$STATE"
    echo "yacer-local: $BRANCH carries no overlay files and no tree-patches." >&2
    echo "  Building from this would produce stock CoreELEC." >&2
    exit 1
  fi
  echo "applied $BRANCH @ ${commit:0:9} - build as usual, then 'yacer-local.sh revert'"
  ;;

revert)
  [ -f "$STATE" ] || die "nothing applied"
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  commit=$(sed -n 's/^commit //p' "$STATE")
  git archive "$commit" | tar -x -C "$tmp"

  # Check before touching anything. These files are untracked in a mirror
  # checkout, so git holds no copy and rm is final: a patch edited in the build
  # tree - regenerated after a fix, say - would be destroyed by a routine revert
  # that reported success. Refusing half way through would leave the tree part
  # reverted, so this runs first.
  edited=""
  while read -r rel; do
    [ -n "$rel" ] || continue
    [ -f "$rel" ] || continue
    cmp -s "$tmp/overlay/$rel" "$rel" || edited="$edited $rel"
  done < <(sed -n 's/^file //p' "$STATE")
  if [ -n "$edited" ] && [ "${2:-}" != "--force" ]; then
    echo "yacer-local: these have been edited since they were applied:" >&2
    for f in $edited; do echo "    $f" >&2; done
    echo "  Copy them into the yacer checkout and commit, or re-run with --force" >&2
    echo "  to discard them. Nothing has been changed." >&2
    exit 1
  fi

  # Reverse the patches while the files still look the way they did. A failure
  # here leaves our change sitting in an upstream-owned file, so it has to stop:
  # carrying on would delete the state file - the only record of what was applied
  # - and report a clean tree that is not clean.
  failed=""
  while read -r n; do
    [ -n "$n" ] || continue
    if git apply -R --whitespace=nowarn "$tmp/tree-patches/$n"; then
      echo "  unpatched $n"
    elif git apply --check --whitespace=nowarn "$tmp/tree-patches/$n" 2>/dev/null; then
      # it applies forward, so it is simply not in the tree any more - someone
      # restored the file from git rather than undoing their edit. Nothing to do.
      echo "  already absent $n"
    else
      failed="$failed $n"
    fi
  done < <(tac "$STATE" | sed -n 's/^patch //p')
  if [ -n "$failed" ]; then
    echo "yacer-local: could not reverse:" >&2
    for n in $failed; do echo "    $n" >&2; done
    echo "  The tree still carries those changes. Keeping $STATE so the record of" >&2
    echo "  what was applied survives; undo them by hand, then revert again." >&2
    exit 1
  fi

  sed -n 's/^file //p' "$STATE" | while read -r rel; do
    rm -f "$rel"
    d=$(dirname "$rel")
    while [ "$d" != "." ] && rmdir "$d" 2>/dev/null; do d=$(dirname "$d"); done
    echo "  removed $rel"
  done

  rm -f "$STATE"
  echo "reverted - this is a plain checkout of the mirror again"
  ;;

*) die "usage: yacer-local.sh [apply|revert|status]" ;;
esac
