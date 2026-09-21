#!/usr/bin/env bash
#
# Assemble the build tree: an upstream checkout with our work laid on top.
#
# Three mechanisms, deliberately kept apart:
#
#   patches-yacer/ our patches to packages, grouped by what they are for and
#                  copied into the project the build is for - where
#                  scripts/unpack looks for them. See scripts/yacer-patches.sh.
#
#   overlay/       other files we ADD. A path that already exists upstream is
#                  refused rather than overwritten - overwriting silently reverts
#                  whatever upstream changed in that file, with nothing to notice
#                  it.
#
#   tree-patches/  changes to a file upstream OWNS. Must apply cleanly; if
#                  upstream moved the code the patch stops the build instead of
#                  quietly restoring our older copy.
#
# Run from the root of the upstream checkout.
set -euo pipefail

SRC=${1:-.yacer}
[ -d "$SRC" ] || { echo "::error::yacer source $SRC not found"; exit 1; }

# shellcheck source=yacer-patches.sh
. "$(dirname "${BASH_SOURCE[0]}")/yacer-patches.sh"

# A patch that cannot land the way it reads stops the build here, where the
# message is about the branch, rather than as a package that quietly misses it.
if ! check_patches "$SRC"; then
  echo "::error::our patches do not check out - refusing to assemble"
  exit 1
fi

# ------------------------------------------------------------------- added
added=0
collisions=0
check_one() {
  if [ -e "$2" ]; then
    echo "::error file=$2::we would overwrite a file upstream already has."
    echo "         If upstream adopted this change, delete ours. If the name"
    echo "         clashes, rename ours. To modify it, use tree-patches/."
    collisions=$((collisions + 1))
  fi
}
each_file check_one "$SRC"

if [ "$collisions" -gt 0 ]; then
  echo "::error::$collisions file(s) collide with upstream - refusing to assemble"
  exit 1
fi

add_one() {
  mkdir -p "$(dirname "$2")"
  cp -p "$1" "$2"
  echo "  added   $2"
  added=$((added + 1))
}
each_file add_one "$SRC"

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
  echo "::error::nothing was assembled - no files added and no tree-patches."
  echo "         A build from this tree would be stock CoreELEC published as ours."
  exit 1
fi
echo "assembled: $added file(s) added, $applied patch(es) applied"
