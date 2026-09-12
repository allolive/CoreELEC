#!/usr/bin/env bash
#
# Assemble the build tree: an upstream checkout with our work laid on top.
#
# Two mechanisms, deliberately kept apart:
#
#   overlay/       files we ADD. A path that already exists upstream is refused
#                  rather than overwritten - overwriting silently reverts whatever
#                  upstream changed in that file, with nothing to notice it.
#
#   tree-patches/  changes to a file upstream OWNS. Must apply cleanly; if
#                  upstream moved the code the patch stops the build instead of
#                  quietly restoring our older copy.
#
# Run from the root of the upstream checkout.
set -euo pipefail

SRC=${1:-.yacer}
[ -d "$SRC" ] || { echo "::error::overlay source $SRC not found"; exit 1; }

# ---------------------------------------------------------------- overlay
added=0
collisions=0
if [ -d "$SRC/overlay" ]; then
  while IFS= read -r -d '' f; do
    rel=${f#"$SRC/overlay/"}
    if [ -e "$rel" ]; then
      echo "::error file=$rel::overlay would overwrite a file upstream already has."
      echo "         If upstream adopted this change, delete ours. If the name"
      echo "         clashes, rename ours. To modify it, use tree-patches/."
      collisions=$((collisions + 1))
    fi
  done < <(find "$SRC/overlay" -type f -print0)

  if [ "$collisions" -gt 0 ]; then
    echo "::error::$collisions overlay file(s) collide with upstream - refusing to assemble"
    exit 1
  fi

  while IFS= read -r -d '' f; do
    rel=${f#"$SRC/overlay/"}
    mkdir -p "$(dirname "$rel")"
    cp -p "$f" "$rel"
    echo "  added   $rel"
    added=$((added + 1))
  done < <(find "$SRC/overlay" -type f -print0)
fi

# ---------------------------------------------------------- tree-patches
applied=0
shopt -s nullglob
for p in "$SRC"/tree-patches/*.patch; do
  # --3way would let a patch apply against moved context by guessing; that is
  # exactly the silent drift this is meant to catch, so plain apply only.
  if git apply --whitespace=nowarn "$p"; then
    echo "  patched $(basename "$p")"
    applied=$((applied + 1))
  else
    echo "::error file=$(basename "$p")::patch no longer applies - upstream changed"
    echo "         the file it edits. Rebase the patch, or drop it if upstream"
    echo "         has fixed the same thing."
    exit 1
  fi
done
shopt -u nullglob

if [ "$added" -eq 0 ] && [ "$applied" -eq 0 ]; then
  echo "::error::nothing was assembled - no overlay files and no tree-patches."
  echo "         A build from this tree would be stock CoreELEC published as ours."
  exit 1
fi
echo "assembled: $added file(s) added, $applied patch(es) applied"
