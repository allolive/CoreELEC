# Yet Another CoreELEC Repo

Unofficial CoreELEC builds for Amlogic devices.

> **Not for general use.** These are unattended builds of a personal tree, made
> for one Ugoos AM6B Plus. Most are never installed anywhere and none carry a
> testing promise. The repository is public because a fork has to be. If you want
> CoreELEC, take an [official release](https://coreelec.org) instead.

Each build is the standard `Amlogic-no.aarch64` update tar - the same one
upstream ships - so it installs on any box CoreELEC 22 supports. The device
tree is chosen per box at boot and nothing here is tied to a particular model.

Update tars only; fresh-flash images are not published.

## Branches

- **`coreelec-22`** — an exact mirror of `CoreELEC/CoreELEC`. Nothing of ours is
  committed there. It is force-pushed to follow upstream wherever they go,
  including across a history rewrite.
- **`yacer`** — this branch. It shares no history with the mirror and holds only
  the files we add or replace.

The build checks out the mirror, lays this branch over the top, and builds the
result. Our files are never replayed onto upstream, so an upstream rewrite cannot
conflict with them.

## Layout

- **`overlay/`** — files we add, copied into the tree as-is. A path that already
  exists upstream is refused rather than overwritten: replacing a whole upstream
  file would silently revert whatever they changed in it, with nothing to notice.
- **`tree-patches/`** — the few upstream files we do change, as patches. If
  upstream moves that code the patch stops applying and the build stops with it,
  rather than quietly restoring our older copy.

Our own patches to packages go in `overlay/projects/.../patches-yacer/<pkg>/`,
beside upstream's `patches/` rather than scattered through them, and are applied
the same way upstream's are. They show up in the build log as `(yacer)` so it is
clear which are ours.

Currently one: HDR10+ to Dolby Vision conversion for the Amlogic decoder.

## Building locally

Two checkouts of the same repository, so the branch can be edited while the build
tree stays a build tree:

    ~/Documents/coreelec/CoreELEC    the coreelec branches - the build tree
    ~/Documents/coreelec/CoreELEC-yacer  this branch - where our patches live

    git -C CoreELEC worktree add ../CoreELEC-yacer yacer     # once

Write patches in the `yacer` checkout and commit them there. Then, in the
CoreELEC checkout:

    ./scripts/yacer-local.sh apply     # lay the branch over the build tree
    ./build-ugoos-am6b-plus.sh         # unchanged
    ./scripts/yacer-local.sh revert    # back to a plain mirror checkout

`apply` reads the branch straight out of the shared repository, so the two
checkouts never need to be copied between. It refuses to start from a build tree
that is not at the mirror state the tree-patches were written against, refuses a
dirty copy of anything they edit, and refuses a patch that also sits loose in the
package's own patch directory - which would otherwise be applied twice.

## Releases

Every build uploads its update `.tar` and checksum to a single `builds` release -
one tag, not one per build. The file name carries the date and both commits, so
builds stay distinguishable without a tag each. The `yacer-feed` release holds the
catalogue the box reads, rebuilt from the tars that are actually there.

Source archives are mirrored on the `packages-mirror` release and seeded before
the build, so a build does not depend on third-party hosts staying reachable.

## License

Our original code is released under GPLv2.

## Copyright

As CoreELEC includes code from many upstream projects it includes many copyright owners. CoreELEC makes NO claim of copyright on any upstream code. Patches to upstream code have the same license as the upstream project, unless specified otherwise. For a complete copyright list please checkout the source code to examine license headers. Unless expressly stated otherwise all code submitted to the CoreELEC project (in any form) is licensed under GPLv2. You are absolutely free to retain copyright. To retain copyright simply add a copyright header to each submitted code page. If you submit code that is not your own work it is your responsibility to place a header stating the copyright.
