# Yet Another CoreELEC Repo

Unofficial CoreELEC builds for Amlogic devices.

> **Tracks the CoreELEC 22.0 nightly tree, with a few patches of our own.**
> Builds install like any CoreELEC update - copy the `.tar` to your box's
> `.update` share and reboot. Each release lists what changed since the one
> before it. This is not an official CoreELEC release and carries no warranty:
> you install it at your own risk.

## Why use these builds instead of an official one

One reason: the patches in the table below. Everything else is CoreELEC exactly
as they ship it, built from their nightly tree. If none of these matter to you,
take an [official release](https://coreelec.org) - it is the same software with
fewer moving parts.

| Patch | What it does |
| --- | --- |
| **Buffering over a slow network** | A share that stops answering part way through a file ends playback instead of showing the buffering wheel, and a merely slow one limps on dropping frames. Kodi reads a stall as the end of the file in several places at once, and the check that should start buffering waits for a condition that never arrives once the picture has stopped. These patches attempt to tell a stall apart from a file that has genuinely ended, and to buffer on the cache running dry instead, so playback pauses and picks up again when the share answers. A share that is gone for good still stops, after about two minutes. |
| **HDR10+ to Dolby Vision** | Kodi's existing "Tone map HDR to Dolby Vision" does not read that metadata at all - the VS10 engine tone maps from the static HDR10 values, so every scene is graded from one set of numbers chosen for the whole file. This attempts to read the per-scene metadata the file carries and emit Dolby Vision profile 8.1 from it, so the brightness the display works to follows the content. Closer to the master on anything with large swings between scenes; on a file graded flat you will see little.<br><br>What it does not do: HDR10+'s tone curve has no Dolby Vision equivalent and is not translated - only the per-scene brightness levels are carried across. CM v2.9 additionally has no field for an average below 819 codewords, which CM v4.0 carries in a level 3 offset. So this is a conversion of the metadata that has somewhere to go, not a lossless one. |
| **Audio sync on passthrough** | With bitstreamed audio - Dolby TrueHD, DTS-HD, Atmos - sound and picture are only ever brought into average sync, never actually aligned, and the offset drifts as playback goes on. Pausing, resuming or skipping changes the drift again when playback picks up, so it settles somewhere different each time. Kodi has no correction fine enough to take it out on a passthrough stream.<br><br>The video smoothness suffers for it too. Every time the player resynchronises it steps the master clock, and a step lands as a frame repeated or a frame dropped creating a visible hitch. These anomalies are infrequent but noticeable: they arrive with the corrections rather than steadily, so the same sequence can play clean one time and hitch the next.<br><br>This attempts to hold the two together by trimming the audio clock very slightly as it plays, so the offset stops wandering and the corrections stop with it. The kernel has to allow that trim, so this series carries driver patches as well - and it only works on the chips those patches cover: **S905X2/D2/Y2, S922X, A311D and S905X3/D3** (g12a, g12b, sm1). On any other Amlogic chip the setting is there but does nothing.<br><br>Off by default: **Settings → CoreELEC → Audio passthrough A/V sync**, at Light, Medium or Strong. A stronger setting closes a large gap sooner, not more accurately. No effect on PCM output. Some receivers and DACs may struggle to follow a shifted clock. If yours clicks, mutes or drops out, lower the strength or turn it off. |

## Which devices

Each build is the standard `Amlogic-no.aarch64` update tar - the same one
upstream ships - so it runs on any **Amlogic** box CoreELEC 22 supports:
S905X/X2/X3/X4/X5, S912, S922X/A311D, S928X and the rest of the `Amlogic-no`
family. The device tree is chosen per box at boot, so nothing here is tied to a
particular model. Built and tested on a Ugoos AM6B Plus (S922X-J); every other
board gets the same image and no specific attention. Audio sync is narrower than
the rest - it needs a kernel change that exists only on g12a, g12b and sm1, and
the table says so.

Not for Raspberry Pi, Rockchip, Allwinner or a PC - CoreELEC is Amlogic only.

Update tars only; fresh-flash images are not published. To start from nothing,
install an [official CoreELEC release](https://coreelec.org) first, then apply a
build from here on top.

## Updating

Copy the `.tar` from any release to the `.update` share and reboot - the same
way an official update is applied.

Or let the box do it. In **Settings -> CoreELEC -> Updates**:

- **Update Channel** - `YACER-22` is offered without any custom-channel setup.
  Trains the box cannot move to are not listed.
- **Automatic Updates** - set to `auto` and the box installs each new build as
  it appears, checking this repository rather than the official update server.
- **Available Versions** - pick a specific build by hand, including an older one.

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

The ones in the table above, plus what is not a feature in its own right: guards
in the Amlogic video path that the audio work is built on, and an updater that
follows the builds in this repository - offering the train the box is actually
running rather than the ones upstream happens to publish, and checking here
rather than the official server when automatic updates are on.

## Building locally

Two checkouts of the same repository, so the branch can be edited while the build
tree stays a build tree:

    ~/Documents/coreelec/CoreELEC    the coreelec branches - the build tree
    ~/Documents/coreelec/CoreELEC-yacer  this branch - where our patches live

    git -C CoreELEC worktree add ../CoreELEC-yacer yacer     # once

Write patches in the `yacer` checkout. Then, in the CoreELEC checkout:

    ../CoreELEC-yacer/scripts/yacer-local.sh apply         # lay the branch over the tree
    ../CoreELEC-yacer/scripts/yacer-build-local.sh        # build it
    ../CoreELEC-yacer/scripts/yacer-local.sh revert       # back to a plain mirror

`yacer-build-local.sh` runs the same `make release` CI does, with two things a
plain `make` gets wrong on a workstation: it keeps the compiler cache outside
the build tree, where `make distclean` cannot delete it, and it stops up front
if `libcrypt-dev` is absent rather than letting `heimdal:host` fail to link a
quarter of an hour in. `TARGET=image` adds the fresh-flash `.img.gz`; `CLEAN=1`
starts from a distclean.

`apply` takes the files from the `yacer` checkout as they are, so an edit can be
built without being committed first - `YACER_SOURCE=git` forces the committed
tree instead, for building exactly what CI will. It keeps a copy of what it laid
down so `revert` removes exactly that. It refuses to start from a build tree
that is not at the mirror state the tree-patches were written against, refuses a
dirty copy of anything they edit, and refuses a patch that also sits loose in the
package's own patch directory - which would otherwise be applied twice.

## Releases

Every build gets its own tag and release, carrying the update `.tar`, its
checksum, and links to the two commits it was built from. Nothing is ever
deleted: an old build stays downloadable from its own release page for as long
as the repository exists.

The `yacer-feed` release holds the catalogue the box reads, rebuilt from the
releases that are actually there. Every build is listed by default; one number in
the feed workflow caps it, and raising that cap brings older ones back, since
they were never removed.

Source archives are mirrored on the `packages-mirror` release and seeded before
the build, so a build does not depend on third-party hosts staying reachable.

## License

Our original code is released under GPLv2.

## Copyright

As CoreELEC includes code from many upstream projects it includes many copyright owners. CoreELEC makes NO claim of copyright on any upstream code. Patches to upstream code have the same license as the upstream project, unless specified otherwise. For a complete copyright list please checkout the source code to examine license headers. Unless expressly stated otherwise all code submitted to the CoreELEC project (in any form) is licensed under GPLv2. You are absolutely free to retain copyright. To retain copyright simply add a copyright header to each submitted code page. If you submit code that is not your own work it is your responsibility to place a header stating the copyright.
