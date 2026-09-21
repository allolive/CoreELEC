# Where our patches live on the branch, where they land in the build tree, and
# what makes a set of them valid. Sourced by yacer-assemble.sh (CI) and
# yacer-local.sh (workstation) so the two cannot drift apart.
#
# The branch groups them by what they are for:
#
#   patches-yacer/<NN-component-feature>/<package>/<nn-title>.patch
#
# A group may also hold a README.md: a heading naming it in prose and a short
# description of what its patches are for, which is what the change list shows.
# It is ours, not the build's, so it is checked but never copied into the tree.
#
# The build wants them by package alone, so the patches land flat in
# projects/Amlogic-ce/patches-yacer/<package>/, named <group><nn-title>.patch:
# the group number is composed in on the way rather than written into every
# filename. Renaming or renumbering a group is then renaming one directory,
# not every patch in it. Order still comes from the name the build sees, so a
# group's number decides where its patches sit among the other groups', and a
# patch's own number where it sits inside its group.

PATCHES_DEST=projects/Amlogic-ce/patches-yacer

# Run $1 for every file the branch copy in $2 adds, as:
#   callback <source path> <path in the tree>
# A patch is placed by the directory it sits in, so a copy taken before the
# groups existed reads the same way.
each_file() {
  local cb=$1 root=$2 f
  if [ -d "$root/patches-yacer" ]; then
    local pkg grp
    while IFS= read -r -d '' f; do
      [ "$(basename "$f")" = README.md ] && continue   # describes the group
      pkg=$(basename "$(dirname "$f")")
      grp=$(basename "$(dirname "$(dirname "$f")")")
      "$cb" "$f" "$PATCHES_DEST/$pkg/${grp%%-*}_$(basename "$f")"
    done < <(find "$root/patches-yacer" -type f -print0)
  fi
  if [ -d "$root/overlay" ]; then
    while IFS= read -r -d '' f; do
      "$cb" "$f" "${f#"$root/overlay/"}"
    done < <(find "$root/overlay" -type f -print0)
  fi
}

# Refuse a set of patches that would not land the way it reads. Run from the
# root of the build tree: the package names are checked against what is in it,
# because a patch in a directory named after no package is never looked for by
# anything - unpack would simply not find it, and the build would say nothing.
check_patches() {
  local root=$1 f rel group pkg name num dest key bad=0 packages
  [ -d "$root/patches-yacer" ] || return 0

  packages=$(find packages projects/*/packages -name package.mk -printf '%h\n' 2>/dev/null \
             | sed 's|.*/||' | sort -u)

  local -A group_of=() first_at=()
  while IFS= read -r -d '' f; do
    rel=${f#"$root/patches-yacer/"}
    case "$rel" in
      */README.md) continue ;;   # describes its group; not copied into the tree
      */*/*/*) echo "yacer: $rel is too deep - a patch goes in <group>/<package>/" >&2
               bad=1; continue ;;
      */*/*)   ;;
      *)       echo "yacer: $rel is not in a <group>/<package>/ directory" >&2
               bad=1; continue ;;
    esac
    group=${rel%%/*}
    pkg=${rel#*/}; pkg=${pkg%%/*}
    name=${rel##*/}

    # The group's number is composed into every destination name, so a group
    # without one would put its patches wherever the rest of the name sorted.
    case "$group" in
      [0-9][0-9]-*) ;;
      *) echo "yacer: group $group has no number - it decides where its" >&2
         echo "       patches apply among the other groups'." >&2
         bad=1; continue ;;
    esac

    case "$name" in
      *.patch|*.patch.*) ;;
      *) echo "yacer: $rel is not a patch - only patches are copied into the tree" >&2
         bad=1; continue ;;
    esac

    if ! grep -qxF "$pkg" <<<"$packages"; then
      echo "yacer: $rel - no package named '$pkg' in this tree, so nothing would" >&2
      echo "       ever look for that patch. Check the spelling." >&2
      bad=1; continue
    fi

    # Everything in a package lands in one directory, named for its group and
    # its own number. Two patches of a group with the same name, or the same
    # number, and one is lost or the order they apply in stops being the order
    # they read in.
    dest="$pkg/${group%%-*}_$name"
    if [ -n "${group_of[$dest]:-}" ]; then
      echo "yacer: $name is twice in $group for $pkg - it lands once" >&2
      bad=1
    fi
    group_of[$dest]=$group

    num=${name%%-*}
    case "$num" in ''|*[!0-9]*) num="" ;; esac
    if [ -z "$num" ]; then
      echo "yacer: $rel does not start with a number - it decides where the" >&2
      echo "       patch applies inside its group." >&2
      bad=1
    else
      key="$group/$pkg/$num"
      if [ -n "${first_at[$key]:-}" ] && [ "${first_at[$key]}" != "$rel" ]; then
        echo "yacer: $group has two $pkg patches numbered $num - renumber one:" >&2
        echo "       ${first_at[$key]}" >&2
        echo "       $rel" >&2
        bad=1
      else
        first_at[$key]=$rel
      fi
    fi
  done < <(find "$root/patches-yacer" -type f -print0)
  return $bad
}
