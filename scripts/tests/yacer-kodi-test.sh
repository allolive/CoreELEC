#!/usr/bin/env bash
#
# Scenarios for yacer-kodi.sh against fake remotes in a temporary directory:
# allolive/xbmc, allolive/CoreELEC (yacer + coreelec-22), CoreELEC/CoreELEC and
# CoreELEC/xbmc. Nothing outside $TMPDIR is touched.
#
#   yacer-kodi-test.sh [scenario...]
set -euo pipefail

SRC=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ROOT=$(mktemp -d)
[ -n "${KEEP:-}" ] || trap 'rm -rf "$ROOT"' EXIT
R=$ROOT/state
LOG=$ROOT/log

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_EDITOR=true LC_ALL=C
export GIT_AUTHOR_NAME=allolive GIT_AUTHOR_EMAIL=160342668+allolive@users.noreply.github.com
export GIT_COMMITTER_NAME=$GIT_AUTHOR_NAME GIT_COMMITTER_EMAIL=$GIT_AUTHOR_EMAIL
export GIT_ADVICE=0
unset GITHUB_ACTIONS GITHUB_OUTPUT GITHUB_STEP_SUMMARY RUNNER_TEMP YACER_CI GH_TOKEN XBMC_DEPLOY_KEY

XB=$R/xbmc; CE=$R/ce; CY=$R/cy; RM=$R/remotes
export HOME=$R/home XDG_CONFIG_HOME=$R/xdg
export YACER_XB=$XB YACER_CE=$CE YACER_CY=$CY
export YACER_URL_XBMC=file://$RM/aoxbmc.git YACER_URL_CXBMC=file://$RM/cexbmc.git
export YACER_URL_CE=file://$RM/aoce.git YACER_URL_UP=file://$RM/cece.git
export YACER_PUSH_XBMC=file://$RM/aoxbmc.git

Y=overlay/projects/Amlogic-ce/patches-yacer/kodi
KPKG=projects/Amlogic-ce/packages/mediacenter/kodi
K2=projects/Amlogic-ce/patches/kodi
SOB="Signed-off-by: allolive <160342668+allolive@users.noreply.github.com>"

yk()   { bash "$CY/scripts/yacer-kodi.sh" "$@"; }
say()  { echo "$*" >> "$LOG"; }
check() { if eval "$1"; then :; else echo "  check failed: $1" | tee -a "$LOG" >&2; return 1; fi; }
must_fail() { # pattern cmd...
  local pat=$1 out; shift
  if out=$("$@" 2>&1); then echo "$out" >> "$LOG"; echo "  expected failure: $*" >&2; return 1; fi
  echo "$out" >> "$LOG"
  grep -qiE -- "$pat" <<<"$out" || { echo "  failure did not say '$pat': $*"$'\n'"$out" >&2; return 1; }
}
lines() { local i; for i in $(seq 1 40); do echo "$1 line $i"; done; }
setline() { sed -i "$2s/.*/$3/" "$1"; }
refs() { git -C "$RM/aoxbmc.git" show-ref; git -C "$RM/aoce.git" show-ref; }
tip()  { git -C "$RM/aoxbmc.git" rev-parse yacer-kodi; }
ce_show() { git -C "$RM/aoce.git" show "$1"; }
active() { git -C "$RM/aoce.git" ls-tree --name-only yacer "$Y/" | sed 's#.*/##' | grep '\.patch$' || true; }
allfiles() { git -C "$RM/aoce.git" ls-tree --name-only yacer "$Y/" | sed 's#.*/##'; }
kodiid() {
  local out="" d id
  for d in "$KPKG" "$K2" projects/Amlogic-ce/devices/Amlogic-no/patches/kodi; do
    id=$(git -C "$1" rev-parse -q --verify "$2:$d" 2>/dev/null) || id=-
    out+="${out:+ }$id"
  done; echo "$out"
}

# --------------------------------------------------------------- fixture
fixture() {
  mkdir -p "$RM"
  git init -q --bare -b aml "$RM/cexbmc.git"; git -C "$RM/cexbmc.git" config uploadpack.allowAnySHA1InWant true
  git init -q --bare -b coreelec-22 "$RM/cece.git"
  git init -q --bare -b yacer-kodi "$RM/aoxbmc.git"; git -C "$RM/aoxbmc.git" config uploadpack.allowAnySHA1InWant true
  git init -q --bare -b yacer "$RM/aoce.git"; git -C "$RM/aoce.git" config receive.shallowUpdate true
  git -C "$RM/aoce.git" config uploadpack.allowAnySHA1InWant true
  git -C "$RM/aoxbmc.git" config receive.denyNonFastForwards true
  git -C "$RM/aoxbmc.git" config receive.denyDeletes true
  mkdir -p "$HOME" "$XDG_CONFIG_HOME"
  cat > "$RM/aoce.git/hooks/pre-receive" <<EOF
#!/bin/sh
rc=0 z=0000000000000000000000000000000000000000
while read -r o n ref; do
  [ "\$ref" = refs/heads/yacer ] || continue
  [ ! -f "$ROOT/reject-yacer" ] || { echo "yacer push rejected (test)"; rc=1; }
  [ "\$n" != "\$z" ] || { echo "yacer may not be deleted"; rc=1; }
  [ "\$o" = "\$z" ] || [ "\$n" = "\$z" ] || git merge-base --is-ancestor "\$o" "\$n" || { echo "yacer is never rewritten"; rc=1; }
done
exit \$rc
EOF
  chmod +x "$RM/aoce.git/hooks/pre-receive"

  # CoreELEC/xbmc
  git init -q -b aml "$R/wk"; cd "$R/wk"
  mkdir xbmc
  for f in FileCache Demux AMLCodec Video; do lines "$f" > "xbmc/$f.cpp"; done
  lines libdvd > libdvd.txt
  git add -A; git commit -qm "kodi 22"
  git remote add origin "file://$RM/cexbmc.git"; git push -q origin aml
  PIN0=$(git rev-parse HEAD)

  # CoreELEC/CoreELEC
  git init -q -b coreelec-22 "$R/wc"; cd "$R/wc"
  mkdir -p "$KPKG" "$K2"
  printf 'PKG_NAME="kodi"\nPKG_VERSION="%s"\nPKG_SHA256=""\n' "$PIN0" > "$KPKG/package.mk"
  (cd "$R/wk" && setline libdvd.txt 3 "libdvd line 3 quiet" && git diff && git checkout -q libdvd.txt) > "$K2/kodi-05-libdvd.patch"
  git add -A; git commit -qm "CoreELEC 22"
  git remote add origin "file://$RM/cece.git"; git push -q origin coreelec-22

  # our patches, made on pin0 + kodi-05
  git init -q "$R/mk"; cd "$R/mk"
  git fetch -q "file://$RM/cexbmc.git" aml; git checkout -q --detach FETCH_HEAD
  patch -s -p1 < "$R/wc/$K2/kodi-05-libdvd.patch"; git commit -qam base
  mkp() { local m=$1 f=$2; shift 2; while [ $# -gt 0 ]; do setline "$f" "$1" "$2"; shift 2; done; git commit -qam "$m" -m "Short body." -m "$SOB"; }
  mkp "ProcessInfo: publish whether the codec buffers" xbmc/Video.cpp 5 "Video line 5 ours"
  mkp "FileCache: a stalled source is not the end of the file" xbmc/FileCache.cpp 10 "FileCache line 10 ours"
  mkp "DVDDemuxFFmpeg: don't end playback when a share is slow" xbmc/Demux.cpp 10 "Demux line 10 ours" 30 "Demux line 30 ours"
  mkp "AMLCodec: bound the header copies" xbmc/AMLCodec.cpp 10 "AMLCodec line 10 ours"
  mkp "AMLCodec: don't busy-spin on EAGAIN" xbmc/AMLCodec.cpp 30 "AMLCodec line 30 ours"
  mkdir "$R/p"; git format-patch -q --full-index -o "$R/p" HEAD~5..HEAD
  git checkout -q HEAD~5; mkp "VideoPlayer: an old idea" xbmc/Video.cpp 20 "Video line 20 old"
  git format-patch -q --full-index --start-number 6 -o "$R/p" HEAD~1..HEAD
  cd "$R/p"
  mv 0001-* 0010-ProcessInfo-publish-whether-the-codec-buffers.patch
  mv 0002-* 0011-FileCache-a-stalled-source.patch
  mv 0003-* 0017-DVDDemuxFFmpeg-slow-share.patch
  mv 0004-* 1025-AMLCodec-bound-the-header-copies.patch
  mv 0005-* 1026-AMLCodec-busy-spin.patch
  mv 0006-* 0012-VideoPlayer-an-old-idea.patch.disabled
  sed -i '1d' 0010-*.patch
  sed -i '/^Date: /d' 1026-*.patch

  # allolive/CoreELEC
  git -C "$R/wc" push -q "file://$RM/aoce.git" coreelec-22
  git init -q -b yacer "$R/wy"; cd "$R/wy"
  mkdir -p scripts "$Y"; cp "$SRC/scripts/yacer-kodi.sh" scripts/; cp "$R/p"/* "$Y"/
  git add -A; git commit -qm "yacer"
  git push -q "file://$RM/aoce.git" yacer
  cd "$ROOT"; rm -rf "$R/wy" "$R/p" "$R/mk"

  # local clones
  git init -q "$XB"
  git -C "$XB" remote add origin "file://$RM/aoxbmc.git"
  git -C "$XB" remote add coreelec "file://$RM/cexbmc.git"
  git -C "$XB" fetch -q coreelec
  git -C "$XB" config --local rerere.enabled true
  git -C "$XB" config --local user.email "$GIT_AUTHOR_EMAIL"
  git clone -q -b coreelec-22 "file://$RM/aoce.git" "$CE"
  git -C "$CE" remote add upstream "file://$RM/cece.git"
  git -C "$CE" fetch -q upstream
  git -C "$CE" worktree add -q "$CY" yacer
}

bootstrap() {
  yk import >> "$LOG" 2>&1
  git -C "$XB" push -q origin yacer-kodi
  yk patches >> "$LOG" 2>&1
  local out; out=$XB/.git/yacer-patches
  cp "$out"/*.patch "$CY/$Y/"; cp "$out/kodi.source" "$CY/"
  git -C "$CY" add -A; git -C "$CY" commit -qm "kodi: regenerate patches from yacer-kodi" -m "$SOB"
  git -C "$CY" push -q origin yacer
  git -C "$CE" fetch -q origin
}

# upstream: new kodi commit on branch $1 from $2 with message $3, then edits "file line text"...
kodi_up() {
  local br=$1 from=$2 msg=$3; shift 3
  cd "$R/wk"; git checkout -q -B "$br" "$from"
  while [ $# -gt 0 ]; do setline "$1" "$2" "$3"; shift 3; done
  git commit -qam "$msg"; git push -q -f origin "$br"; git rev-parse HEAD; cd "$ROOT"
}
ce_up() { # pin [kodi-05 new line text]
  cd "$R/wc"
  sed -i "s/^PKG_VERSION=.*/PKG_VERSION=\"$1\"/" "$KPKG/package.mk"
  if [ -n "${2:-}" ]; then
    (cd "$R/wk" && git checkout -q "$1" && setline libdvd.txt 3 "$2" && git diff && git checkout -q libdvd.txt) > "$K2/kodi-05-libdvd.patch"
  fi
  git commit -qam "kodi: bump to ${1:0:8}"; git push -q -f origin coreelec-22; cd "$ROOT"
}
ce_k2() { # a CoreELEC kodi patch that sets FileCache line 10, pin unchanged
  local pin
  cd "$R/wc"
  pin=$(sed -n 's/^PKG_VERSION="\(.*\)"/\1/p' "$KPKG/package.mk")
  (cd "$R/wk" && git checkout -q "$pin" && setline xbmc/FileCache.cpp 10 "$1" && git diff && git checkout -q xbmc/FileCache.cpp) > "$K2/kodi-08-filecache.patch"
  git add -A; git commit -qm "kodi: patch FileCache"; git push -q -f origin coreelec-22; cd "$ROOT"
}
cy_push() { git -C "$CY" commit -qm "$1" -m "$SOB"; git -C "$CY" push -q origin yacer; git -C "$CE" fetch -q origin; }
cy_update() { git -C "$CE" fetch -q upstream; git -C "$CE" fetch -q origin; git -C "$CY" merge -q --ff-only origin/yacer; }

WT_=$XB/.git/yacer-work
wt() { (cd "$WT_" && "$@"); }
SYNC_ENV=(); PUSH_ENV=()

# the CI job, split so a test can act between its steps
plan_ci() {
  local ws=$ROOT/runner rc=0
  rm -rf "$ws"; mkdir -p "$ws/tmp"
  git clone -q --depth 1 -b yacer "file://$RM/aoce.git" "$ws/ws" 2>/dev/null
  : > "$ws/out"; : > "$ws/summary"
  PREVTIP=$(tip)
  (cd "$ws/ws" && env "${SYNC_ENV[@]}" YACER_CI=1 RUNNER_TEMP="$ws/tmp" GITHUB_OUTPUT="$ws/out" GITHUB_STEP_SUMMARY="$ws/summary" \
     bash scripts/yacer-kodi.sh plan > "$ws/plan") > "$ws/log" 2>&1 || rc=$?
  PLANRC=$rc; PUSHRC=-
  RESULT=$(sed -n 's/^result=//p' "$ws/plan")
  PLAN=$(cat "$ws/plan"); SLOG=$(cat "$ws/log")
  { echo "--- plan: result=$RESULT rc=$PLANRC"; cat "$ws/plan" "$ws/log"; } >> "$LOG"
}
push_ci() {
  local ws=$ROOT/runner rc=0
  [ "$PLANRC" = 0 ] || return 0
  (cd "$ws/ws" && env "${PUSH_ENV[@]}" YACER_CI=1 RUNNER_TEMP="$ws/tmp" GITHUB_OUTPUT="$ws/out" \
     bash scripts/yacer-kodi.sh push "$ws/plan") > "$ws/pushlog" 2>&1 || rc=$?
  PUSHRC=$rc; SLOG+=$'\n'$(cat "$ws/pushlog")
  { echo "--- push: rc=$PUSHRC"; cat "$ws/pushlog"; } >> "$LOG"
}
sync_ci() {
  plan_ci; push_ci
  if [ "$PLANRC" = 0 ] && [ "$PUSHRC" = 0 ] && [ "$RESULT" != fail ]; then invariants; fi
}

# after every complete sync: the branch and yacer agree
invariants() {
  local t m src
  t=$(tip); m=$(git -C "$RM/aoce.git" rev-parse coreelec-22)
  src=$(ce_show yacer:kodi.source)
  inv() { eval "$1" || { echo "  invariant failed: $1" | tee -a "$LOG" >&2; return 1; }; }
  inv '[ "$(git -C "$RM/aoxbmc.git" rev-parse "$t^{tree}")" = "$(git -C "$RM/aoxbmc.git" rev-parse "$t^2^{tree}")" ]'
  inv '[ "$t" = "$PREVTIP" ] || [ "$(git -C "$RM/aoxbmc.git" rev-parse "$t^1")" = "$PREVTIP" ]'
  inv '[ "$(sed -n "s/^kodi //p" <<<"$src")" = "$(kodiid "$RM/aoce.git" "$m")" ]'
  inv 'git -C "$RM/aoxbmc.git" merge-base --is-ancestor "$(sed -n "s/^branch //p" <<<"$src")" "$t"'
  sed -n 's/^patch \([^ ]*\) \([^ ]*\) .*/\2 \1/p' <<<"$src" | sort > "$ROOT/inv.lines"
  git -C "$RM/aoce.git" ls-tree yacer "$Y/" \
    | awk -F'\t' '$2 ~ /\.patch$/ {n=$2; sub(/.*\//,"",n); split($1,m," "); print n, m[3]}' | sort > "$ROOT/inv.active"
  # every active file is what the sync wrote; a line with no file is a removal the
  # next sync still has to apply
  inv '[ -z "$(comm -23 "$ROOT/inv.active" "$ROOT/inv.lines")" ]'
  if [ "$RESULT" = ok ]; then
    inv 'cmp -s "$ROOT/inv.active" "$ROOT/inv.lines"'
    inv 'grep -qx "branch $t" <<<"$src"'
  fi
}

guard_ci() { # yacer-rev mirror-repo mirror-rev
  local ws=$ROOT/guard rc=0
  rm -rf "$ws"; mkdir -p "$ws"
  git init -q "$ws/m"; git -C "$ws/m" fetch -q --depth 1 "file://$2" "$3"; git -C "$ws/m" checkout -q FETCH_HEAD
  git init -q "$ws/m/.yacer"; git -C "$ws/m/.yacer" fetch -q --depth 1 "file://$RM/aoce.git" "$1"; git -C "$ws/m/.yacer" checkout -q FETCH_HEAD
  git -C "$ws/m/.yacer" remote add origin "file://$RM/aoce.git"
  : > "$ws/out"
  (cd "$ws/m" && GITHUB_ACTIONS=true GITHUB_OUTPUT=$ws/out bash .yacer/scripts/yacer-kodi.sh guard) >> "$LOG" 2>&1 || rc=$?
  GUARDRC=$rc; GUARD=$(sed -n 's/^result=//p' "$ws/out"); GMIRROR=$(sed -n 's/^mirror=//p' "$ws/out")
}

snap()    { rm -rf "$ROOT/snap-$1"; cp -a "$R" "$ROOT/snap-$1"; }
restore() {
  [ -d "$ROOT/snap-$1" ] || { echo "  no snapshot '$1': run the scenario that makes it first" >&2; return 1; }
  rm -rf "$R"; cp -a "$ROOT/snap-$1" "$R"; cd "$ROOT"
}

stack_matches_names() { # C order of active files == stack order on yacer-kodi
  local t want got
  t=$(tip)
  want=$(git -C "$RM/aoxbmc.git" log --reverse --format=%s "$(git -C "$RM/aoxbmc.git" rev-list --first-parent -1 --grep='^yacer-base: ' "$t^2")..$t^2")
  got=$(for f in $(active | sort); do ce_show "yacer:$Y/$f" | git mailinfo -b /dev/null /dev/null | sed -n 's/^Subject: //p'; done)
  [ -n "$want" ] || [ -z "$(active)" ] || return 1
  [ "$got" = "$want" ] || return 1
}
first_parents() { git -C "$RM/aoxbmc.git" rev-list --first-parent --count "$1..$(tip)"; }
mirror_is_upstream() { [ "$(git -C "$RM/aoce.git" rev-parse coreelec-22)" = "$(git -C "$RM/cece.git" rev-parse coreelec-22)" ]; }
land_and_push() { yk land -m "$1" >> "$LOG" 2>&1; git -C "$XB" push -q origin yacer-kodi; }

# ------------------------------------------------------------- scenarios
s_bootstrap() {
  fixture >> "$LOG" 2>&1
  bootstrap
  local t; t=$(tip)
  check '[ "$(git -C "$RM/aoxbmc.git" rev-list --count "$t^2" --not "$t^1")" -eq 6 ]'   # base + 5 patches
  check '[ "$(active | tr "\n" " ")" = "0010-ProcessInfo-publish-whether-the-codec-buffers.patch 0011-FileCache-a-stalled-source.patch 0017-DVDDemuxFFmpeg-slow-share.patch 1025-AMLCodec-bound-the-header-copies.patch 1026-AMLCodec-busy-spin.patch " ]'
  check 'allfiles | grep -qx 0012-VideoPlayer-an-old-idea.patch.disabled'
  check 'ce_show "yacer:$Y/1026-AMLCodec-busy-spin.patch" | grep -q "^Date: "'
  check 'ce_show yacer:kodi.source | grep -qx "branch $t"'
  check '[ "$(ce_show yacer:kodi.source | grep -c ^patch)" -eq 5 ]'
  sync_ci
  check '[ "$RESULT" = ok ] && ! grep -qE "^(xbmc|yacer|mirror)=" <<<"$PLAN"'
  check '[ ! -d "$ROOT/runner/tmp/xbmc" ]'    # nothing changed: no clone
  snap boot
}

s_clean_bump() {
  restore boot
  local old p1; old=$(tip)
  p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream" xbmc/Video.cpp 8 "Video generated with tools")
  ce_up "$p1" "libdvd line 3 quieter"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ]'
  check '[ "$(first_parents "$old")" -eq 1 ]'
  check 'mirror_is_upstream'
  check 'ce_show yacer:kodi.source | grep -qx "kodi $(kodiid "$RM/cece.git" coreelec-22)"'
  check 'stack_matches_names'
  check 'grep -q "yacer .* and coreelec-22" <<<"$SLOG"'
  check '! git -C "$RM/aoce.git" rev-parse -q --verify refs/heads/yacer-mirror-next'
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = ok ] && [ "$GMIRROR" = "$(git -C "$RM/aoce.git" rev-parse coreelec-22)" ]'
  snap bumped
}

s_deploy_key() {
  restore boot
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  PUSH_ENV=(XBMC_DEPLOY_KEY=dummy-secret-key-material YACER_TRACE=1)
  sync_ci
  PUSH_ENV=()
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ]'
  check '[ ! -e "$ROOT/runner/tmp/xbmc-deploy-key" ]'
  check 'grep -qx changed=true "$ROOT/runner/out"'
  check '! grep -q dummy-secret-key-material "$LOG" "$ROOT/runner/pushlog"'
}

conflict_bump() {
  local p2
  p2=$(kodi_up pin2 aml "Propagate failed source seeks in FileCache" xbmc/FileCache.cpp 10 "FileCache line 10 upstream")
  ce_up "$p2"
}

s_conflict_bump() {
  restore boot
  conflict_bump
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && [ "$PUSHRC" = 0 ]'
  check 'refs | cmp -s - "$ROOT/before"'
  check 'grep -q "0011-FileCache-a-stalled-source.patch" <<<"$SLOG"'
  check 'grep -q "xbmc/FileCache.cpp" <<<"$SLOG"'
  check 'grep -q "Propagate failed source seeks in FileCache" <<<"$SLOG"'
  check 'grep -q "::stop-commands::" <<<"$SLOG"'
  check '! grep -qE "^(xbmc|yacer|mirror)=" <<<"$PLAN"'
  guard_ci yacer "$RM/cece.git" coreelec-22
  check '[ "$GUARD" = fail ] && [ "$GUARDRC" = 1 ]'
  snap conflict
  # a build of an older yacer commit that no longer fits is skipped, not failed
  local old; old=$(git -C "$RM/aoce.git" rev-parse yacer)
  echo "note" >> "$CY/$Y/0012-VideoPlayer-an-old-idea.patch.disabled"; git -C "$CY" add -A; cy_push "touch an inactive file"
  guard_ci "$old" "$RM/cece.git" coreelec-22
  check '[ "$GUARD" = skip ] && [ "$GUARDRC" = 0 ]'
}

s_held_mirror_fallback() {
  restore boot
  local m0 landed; m0=$(git -C "$RM/aoce.git" rev-parse coreelec-22)
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "10s/.*/Demux line 10 ours v2/" xbmc/Demux.cpp && git commit -qa --fixup=HEAD~2 && git rebase -q -i --autosquash HEAD~4' >> "$LOG" 2>&1
  land_and_push "kodi: DVDDemuxFFmpeg v2"
  git -C "$CY" mv "$Y/1026-AMLCodec-busy-spin.patch" "$Y/1026-AMLCodec-busy-spin.patch.disabled"
  cy_push "kodi: disable busy-spin"
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = skip ] && [ "$GUARDRC" = 0 ]'
  conflict_bump
  landed=$(tip)
  sync_ci
  check '[ "$RESULT" = conflict ] && [ "$PUSHRC" = 0 ]'
  check '[ "$(git -C "$RM/aoce.git" rev-parse coreelec-22)" = "$m0" ]'
  check '[ "$(first_parents "$landed")" -eq 1 ]'
  check '! active | grep -q 1026'
  check 'ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -q "Demux line 10 ours v2"'
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = ok ]'
  refs > "$ROOT/before"; sync_ci
  check '[ "$RESULT" = conflict ] && refs | cmp -s - "$ROOT/before"'
}

resolve_filecache() { # text for line 10, then git add
  wt bash -c "$(declare -f lines setline); lines FileCache > xbmc/FileCache.cpp && setline xbmc/FileCache.cpp 10 '$1' && git add xbmc/FileCache.cpp" >> "$LOG" 2>&1
}

s_start_upstream() {
  restore conflict
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream"
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto CoreELEC/xbmc pin2"
  check '[ ! -d "$WT_" ] && ! git -C "$XB" rev-parse -q --verify refs/yacer/start'
  local landed; landed=$(tip)
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$(tip)" = "$landed" ]'
  check 'mirror_is_upstream'
  check 'ce_show "yacer:$Y/0011-FileCache-a-stalled-source.patch" | grep -q "ours on upstream"'
  check 'stack_matches_names'
}

s_hand_continue() {
  restore conflict
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream"
  wt git rebase --continue >> "$LOG" 2>&1
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto pin2, finished by hand"
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream && stack_matches_names'
}

s_not_provable_hand_skip() {
  restore boot
  local pe
  pe=$(kodi_up pinE aml "Demux: take the slow-share fix, reworked" xbmc/Demux.cpp 10 "Demux line 10 ours" xbmc/Demux.cpp 30 "Demux line 30 ours" xbmc/Demux.cpp 8 "Demux line 8 upstream")
  ce_up "$pe"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && grep -q "not provably upstream" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  cy_update
  must_fail "not provably upstream" yk start --upstream
  git -C "$CY" mv "$Y/0017-DVDDemuxFFmpeg-slow-share.patch" "$Y/0017-DVDDemuxFFmpeg-slow-share.patch.merged"
  git -C "$CY" commit -qm "kodi: DVDDemuxFFmpeg merged upstream" -m "$SOB"
  wt git rebase --skip >> "$LOG" 2>&1
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto pinE"
  git -C "$CY" push -q origin yacer
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream'
  check 'allfiles | grep -qx 0017-DVDDemuxFFmpeg-slow-share.patch.merged && ! ce_show yacer:kodi.source | grep -q Demux'
}

s_landed_then_conflict() {
  restore conflict
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream"
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto pin2"
  local p6; p6=$(kodi_up pin6 pin2 "FileCache: seek again" xbmc/FileCache.cpp 10 "FileCache line 10 upstream v2")
  ce_up "$p6"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && grep -q "fallback skipped" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  cy_update
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream v2"
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto pin6"
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream && stack_matches_names'
}

s_internal_error() {
  restore boot
  git -C "$CY" mv "$Y/1026-AMLCodec-busy-spin.patch" "$Y/1026-AMLCodec-busy-spin.patch.disabled"
  cy_push "kodi: disable busy-spin"
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  mkdir -p "$ROOT/fakebin"; printf '#!/bin/sh\nexit 2\n' > "$ROOT/fakebin/sort"; chmod +x "$ROOT/fakebin/sort"
  refs > "$ROOT/before"
  SYNC_ENV=(PATH="$ROOT/fakebin:$PATH")
  sync_ci
  SYNC_ENV=()
  check '[ "$RESULT" = fail ] && [ "$PUSHRC" = 0 ] && refs | cmp -s - "$ROOT/before"'
  check '! grep -qE "^(xbmc|yacer|mirror)=" <<<"$PLAN"'
}

s_fail_no_fallback() {
  restore boot
  git -C "$CY" mv "$Y/1026-AMLCodec-busy-spin.patch" "$Y/1026-AMLCodec-busy-spin.patch.disabled"
  cy_push "kodi: disable busy-spin"
  cd "$R/wc"
  printf -- '--- a/libdvd.txt\n+++ b/libdvd.txt\n@@ -20,3 +20,3 @@\n not there\n-nope\n+yes\n not there\n' > "$K2/kodi-07-stale.patch"
  git add -A; git commit -qm stale; git push -q origin coreelec-22; cd "$ROOT"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = fail ] && [ "$PUSHRC" = 0 ] && grep -q "does not apply" <<<"$SLOG"'
  check 'refs | cmp -s - "$ROOT/before"'
  check '! grep -qE "^(xbmc|yacer|mirror)=" <<<"$PLAN"'
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = skip ]'
}

# a land that reworded a patch, pushed but not synced, plus a disable of the old
# file: the drop must never silently leave the patch live under a new name
s_pending_land_then_disable() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c '
    base=$(git rev-parse HEAD~5); mapfile -t cs < <(git rev-list --reverse "$base..HEAD"); git reset -q --hard "$base"
    for c in "${cs[@]}"; do
      git cherry-pick "$c" >/dev/null
      case "$(git log -1 --format=%s)" in
        DVDDemuxFFmpeg*) git commit -q --amend -m "DVDDemuxFFmpeg: keep playing on a slow share" -m "Short body." -m "'"$SOB"'" ;;
      esac
    done' >> "$LOG" 2>&1
  land_and_push "kodi: reword the demux patch"
  git -C "$CY" mv "$Y/0017-DVDDemuxFFmpeg-slow-share.patch" "$Y/0017-DVDDemuxFFmpeg-slow-share.patch.disabled"
  cy_push "kodi: disable the demux patch"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && grep -q "reworded by a land" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  check '! active | grep -q DVDDemux'
  # the way out: put the file back, let the sync rename it, then disable that one
  git -C "$CY" mv "$Y/0017-DVDDemuxFFmpeg-slow-share.patch.disabled" "$Y/0017-DVDDemuxFFmpeg-slow-share.patch"
  cy_push "kodi: put it back until the sync has run"
  sync_ci
  check '[ "$RESULT" = ok ] && active | grep -qx 0017-DVDDemuxFFmpeg-keep-playing-on-a-slow-share.patch && stack_matches_names'
  check '! allfiles | grep -qx 0017-DVDDemuxFFmpeg-slow-share.patch.disabled'
  cy_update
  git -C "$CY" mv "$Y/0017-DVDDemuxFFmpeg-keep-playing-on-a-slow-share.patch" "$Y/0017-DVDDemuxFFmpeg-keep-playing-on-a-slow-share.patch.disabled"
  cy_push "kodi: disable the reworded demux patch"
  sync_ci
  check '[ "$RESULT" = ok ] && ! active | grep -q DVDDemux && ! ce_show yacer:kodi.source | grep -q DVDDemux && stack_matches_names'
}

# the fallback may only rebase onto the base the stack is already on
s_fallback_needs_same_base() {
  restore conflict
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream"
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto pin2"
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream'
  cy_update
  ce_k2 "FileCache line 10 by CoreELEC"
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours over CoreELEC"
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: rebase onto the CoreELEC patch"
  ce_k2 "FileCache line 10 by CoreELEC again"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && grep -q "fallback skipped" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
}

# a commit reworded by hand after the rebase is not a wedge
s_reword_mid_rebase() {
  restore conflict
  must_fail "does not apply" yk start --upstream
  resolve_filecache "FileCache line 10 ours on upstream"
  wt git rebase --continue >> "$LOG" 2>&1
  wt git commit -q --amend -m "AMLCodec: leave the decoder alone on EAGAIN" -m "Short body." -m "$SOB" >> "$LOG" 2>&1
  local out; out=$(yk start 2>&1); echo "$out" >> "$LOG"
  check 'grep -q "reworded or split by hand" <<<"$out"'
  land_and_push "kodi: rebase onto pin2 with a reworded tip"
  sync_ci
  check '[ "$RESULT" = ok ] && active | grep -qx 1026-AMLCodec-leave-the-decoder-alone-on-EAGAIN.patch && stack_matches_names'
}

s_land_twice() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 x/" xbmc/Video.cpp && git commit -qa -m "Video: another thing" -m "'"$SOB"'"' >> "$LOG" 2>&1
  yk land -m "kodi: first" >> "$LOG" 2>&1
  must_fail "not on origin" yk start
  must_fail "not on origin" yk land -m "kodi: second"
}

s_fully_merged() {
  restore boot
  local p3 t1
  p3=$(kodi_up pin3 aml "AMLCodec: bound the header copies (upstreamed)" xbmc/AMLCodec.cpp 10 "AMLCodec line 10 ours")
  ce_up "$p3"
  touch "$ROOT/reject-yacer"
  refs > "$ROOT/before"
  sync_ci
  rm -f "$ROOT/reject-yacer"
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" != 0 ]'
  t1=$(tip)
  check '[ "$(git -C "$RM/aoce.git" rev-parse yacer)" = "$(grep " refs/heads/yacer$" "$ROOT/before" | cut -d" " -f1)" ]'
  check '[ "$(git -C "$RM/aoce.git" rev-parse coreelec-22)" = "$(grep " refs/heads/coreelec-22$" "$ROOT/before" | cut -d" " -f1)" ]'
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ] && [ "$(tip)" = "$t1" ]'
  check 'allfiles | grep -qx 1025-AMLCodec-bound-the-header-copies.patch.merged'
  check '! active | grep -q 1025'
  check 'mirror_is_upstream && stack_matches_names'
  refs > "$ROOT/before"; sync_ci
  check '[ "$RESULT" = ok ] && refs | cmp -s - "$ROOT/before"'
}

s_partial_merge() {
  restore boot
  local pp; pp=$(kodi_up pinP aml "Demux: second half of the slow-share fix" xbmc/Demux.cpp 30 "Demux line 30 ours")
  ce_up "$pp"
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = conflict ] && grep -q "partly merged" <<<"$SLOG" && grep -q "DVDDemuxFFmpeg" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  check 'grep -q "keeping upstream.s version where they overlap" <<<"$SLOG"'
}

# upstream took one hunk of a patch: resolving locally leaves the rest, and the
# regenerated file records which upstream commits took the other part
s_partly_upstream() {
  restore boot
  local pp out p1
  pp=$(kodi_up pinP aml "Demux: second half of the slow-share fix" xbmc/Demux.cpp 30 "Demux line 30 ours")
  ce_up "$pp"
  sync_ci
  check '[ "$RESULT" = conflict ]'
  cy_update
  out=$(yk start --upstream 2>&1); echo "$out" >> "$LOG"
  check 'grep -q "changed beyond context" <<<"$out" && grep -q "keeping upstream.s version where they overlap" <<<"$out"'
  out=$(yk land -m "kodi: rebase onto pinP" 2>&1); echo "$out" >> "$LOG"
  check 'grep -q "recorded Partly-upstream" <<<"$out"'
  git -C "$XB" push -q origin yacer-kodi
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream && stack_matches_names'
  check '[ "$(ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -c "^Partly-upstream: ")" -eq 1 ]'
  check 'ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -q "^Partly-upstream: ${pp:0:7}"'
  check '! ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -q "Demux line 30"'
  check 'ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -q "Demux line 10 ours"'
  p1=$(kodi_up pinP2 pinP "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$(ce_show "yacer:$Y/0017-DVDDemuxFFmpeg-slow-share.patch" | grep -c "^Partly-upstream: ")" -eq 1 ]'
  check 'stack_matches_names'
}

s_disable_conflicting() {
  restore conflict
  cy_update
  git -C "$CY" mv "$Y/0011-FileCache-a-stalled-source.patch" "$Y/0011-FileCache-a-stalled-source.patch.disabled"
  cy_push "kodi: disable FileCache stall"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ] && mirror_is_upstream'
  check '! ce_show yacer:kodi.source | grep -q FileCache'
  check 'allfiles | grep -qx 0011-FileCache-a-stalled-source.patch.disabled'
  check 'stack_matches_names'
  snap disabled
}

s_enable_after_bumps() {
  restore disabled
  local p4
  p4=$(kodi_up pin4 pin2 "FileCache: more upstream work" xbmc/FileCache.cpp 12 "FileCache line 12 upstream")
  ce_up "$p4"
  sync_ci
  check '[ "$RESULT" = ok ]'
  cy_update
  must_fail "resolve" yk enable 0011-FileCache-a-stalled-source.patch.disabled
  wt bash -c "$(declare -f lines setline); lines FileCache > xbmc/FileCache.cpp && setline xbmc/FileCache.cpp 10 'FileCache line 10 ours again' && setline xbmc/FileCache.cpp 12 'FileCache line 12 upstream' && git add xbmc/FileCache.cpp" >> "$LOG" 2>&1
  yk start >> "$LOG" 2>&1
  land_and_push "kodi: enable FileCache stall again"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ]'
  check 'active | grep -q "FileCache-a-stalled-source"'
  check '! allfiles | grep -qx 0011-FileCache-a-stalled-source.patch.disabled'
  check 'stack_matches_names'
}

s_enable_paths() {
  restore boot
  printf -- '--- a/xbmc/Video.cpp\n+++ b/xbmc/Video.cpp\n@@ -1 +1 @@\n-Video line 1\n+Video line 1 plain\n' > "$CY/$Y/0013-plain.patch.disabled"
  git -C "$CY" add -A; git -C "$CY" commit -qm plain -m "$SOB"
  must_fail "not a mail patch" yk enable 0013-plain.patch.disabled
  check '[ ! -d "$WT_" ]'
  ce_show "yacer:$Y/1025-AMLCodec-bound-the-header-copies.patch" | sed 's/^Subject: .*/Subject: [PATCH] AMLCodec: the same change again/' > "$CY/$Y/0099-same.patch.disabled"
  git -C "$CY" add -A; git -C "$CY" commit -qm same -m "$SOB"
  local out; out=$(yk enable 0099-same.patch.disabled 2>&1); echo "$out" >> "$LOG"
  check 'grep -q "already upstream" <<<"$out"'
  sed -e 's/^index .*/index 1111111111111111111111111111111111111111..2222222222222222222222222222222222222222 100644/' \
      -e 's/^ Video line \(.*\)$/ nothing like line \1/' "$CY/$Y/0012-VideoPlayer-an-old-idea.patch.disabled" > "$CY/$Y/0098-nobase.patch.disabled"
  git -C "$CY" add -A; git -C "$CY" commit -qm nobase -m "$SOB"
  must_fail "no 3-way base" yk enable 0098-nobase.patch.disabled
  check '[ ! -d "$XB/.git/worktrees/yacer-work/rebase-apply" ]'
  out=$(yk start 2>&1); echo "$out" >> "$LOG"
  check 'grep -q "ready to land" <<<"$out"'
}

s_insert_consecutive() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'c26=$(git rev-parse HEAD); git reset -q --hard HEAD~1; sed -i "20s/.*/AMLCodec line 20 new/" xbmc/AMLCodec.cpp; git commit -qam "AMLCodec: keep the new thing apart" -m "'"$SOB"'"; git cherry-pick "$c26" >/dev/null' >> "$LOG" 2>&1
  local before; before=$(allfiles)
  land_and_push "kodi: AMLCodec new thing"
  sync_ci
  check '[ "$RESULT" = ok ]'
  check 'active | grep -qx "10251-AMLCodec-keep-the-new-thing-apart.patch"'
  check '[ "$(allfiles | grep -v 10251)" = "$before" ]'
  check 'stack_matches_names'
}

s_reorder() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c "GIT_SEQUENCE_EDITOR=\"sed -i '1{h;d};2G'\" git rebase -q -i HEAD~2" >> "$LOG" 2>&1
  land_and_push "kodi: reorder the AMLCodec patches"
  sync_ci
  check '[ "$RESULT" = ok ]'
  check 'active | grep -qx 1026-AMLCodec-busy-spin.patch && active | grep -qx 1027-AMLCodec-bound-the-header-copies.patch'
  check 'stack_matches_names'
}

s_reword_and_edit() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c '
    base=$(git rev-parse HEAD~5); mapfile -t cs < <(git rev-list --reverse "$base..HEAD"); git reset -q --hard "$base"
    for c in "${cs[@]}"; do
      git cherry-pick "$c" >/dev/null
      case "$(git log -1 --format=%s)" in
        DVDDemuxFFmpeg*) git commit -q --amend -m "DVDDemuxFFmpeg: keep playing on a slow share" -m "Short body." -m "'"$SOB"'" ;;
        "AMLCodec: bound the header copies") sed -i "11s/.*/AMLCodec line 11 ours too/" xbmc/AMLCodec.cpp; git commit -q -a --amend --no-edit ;;
      esac
    done' >> "$LOG" 2>&1
  land_and_push "kodi: reword and edit"
  sync_ci
  check '[ "$RESULT" = ok ]'
  check 'active | grep -qx "0017-DVDDemuxFFmpeg-keep-playing-on-a-slow-share.patch"'
  check 'ce_show "yacer:$Y/1025-AMLCodec-bound-the-header-copies.patch" | grep -q "line 11 ours too"'
  check '[ "$(active | cut -d- -f1 | tr "\n" " ")" = "0010 0011 0017 1025 1026 " ]'
  check 'stack_matches_names'
}

s_subject_chars() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 bs/" xbmc/Video.cpp && git commit -qa -m '"'VideoPlayer: back\\slash path'"' -m "'"$SOB"'"' >> "$LOG" 2>&1
  land_and_push "kodi: a subject with a backslash"
  sync_ci
  check '[ "$RESULT" = ok ] && active | grep -qx 1027-VideoPlayer-back-slash-path.patch && stack_matches_names'
  check 'ce_show yacer:kodi.source | grep -qF "VideoPlayer: back\\slash path"'
  cy_update
  git -C "$CY" mv "$Y/1027-VideoPlayer-back-slash-path.patch" "$Y/1027-VideoPlayer-back-slash-path.patch.disabled"
  cy_push "kodi: disable the backslash one"
  sync_ci
  check '[ "$RESULT" = ok ] && git -C "$RM/aoxbmc.git" log -1 --format=%s yacer-kodi | grep -qF "drop 1027-VideoPlayer-back-slash-path.patch"'
  yk start >> "$LOG" 2>&1 || { cy_update; yk start >> "$LOG" 2>&1; }
  wt bash -c 'sed -i "39s/.*/Video line 39 ds/" xbmc/Video.cpp && git commit -qa -m "Video:  two  spaces" -m "'"$SOB"'"' >> "$LOG" 2>&1
  must_fail "repeated whitespace" yk land -m "kodi: two spaces"
}

s_replay_restores() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 r/" xbmc/Video.cpp && git commit -qa -m "Video: replayed badly" -m "'"$SOB"'"' >> "$LOG" 2>&1
  mkdir -p "$ROOT/badpatch"; printf '#!/bin/sh\nexit 1\n' > "$ROOT/badpatch/setsid"; chmod +x "$ROOT/badpatch/setsid"
  must_fail "replay" env PATH="$ROOT/badpatch:$PATH" bash "$CY/scripts/yacer-kodi.sh" land -m "kodi: replay fails"
  check '[ "$(wt git symbolic-ref HEAD)" = refs/heads/yacer-kodi-work ] && [ -z "$(wt git status --porcelain)" ]'
}

s_attribution() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 a/" xbmc/Video.cpp && git commit -qa -m "Video: helped" -m "Co-authored-by: Claude <noreply@anthropic.com>" -m "'"$SOB"'"' >> "$LOG" 2>&1
  must_fail "attribution" yk land -m "kodi: helped"
  yk abandon >> "$LOG" 2>&1
  git clone -q "file://$RM/aoxbmc.git" "$R/bad" 2>/dev/null
  (
    cd "$R/bad"; t=$(git rev-parse HEAD); git checkout -q --detach HEAD^2
    sed -i "38s/.*/Video line 38 b/" xbmc/Video.cpp
    GIT_AUTHOR_EMAIL=someone@example.com git commit -qa -m "Video: by someone else" -m "$SOB"
    t2=$(git commit-tree HEAD^{tree} -p "$t" -p HEAD -m "kodi: someone" -m "$SOB")
    git push -q origin "$t2:refs/heads/yacer-kodi"
  ) >> "$LOG" 2>&1
  local tb; tb=$(tip)
  refs > "$ROOT/before"
  sync_ci
  check '[ "$RESULT" = fail ] && grep -q "not authored" <<<"$SLOG" && refs | cmp -s - "$ROOT/before" && [ "$(tip)" = "$tb" ]'
}

s_missed_bumps() {
  restore boot
  local old p1 p1c; old=$(tip)
  p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  p1c=$(kodi_up pin1c pin1 "Video: more" xbmc/Video.cpp 36 "Video line 36 upstream")
  ce_up "$p1c" "libdvd line 3 again"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$(first_parents "$old")" -eq 1 ] && mirror_is_upstream'
}

s_coreelec_only() {
  restore boot
  local old pin; old=$(tip)
  pin=$(sed -n 's/^PKG_VERSION="\(.*\)"/\1/p' "$R/wc/$KPKG/package.mk")
  ce_up "$pin" "libdvd line 3 patched differently"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$(first_parents "$old")" -eq 1 ] && mirror_is_upstream'
  old=$(tip)
  (cd "$R/wc" && sed -i 's/^PKG_SHA256=.*/PKG_SHA256="abc"/' "$KPKG/package.mk" && git commit -qam "kodi: sha" && git push -q origin coreelec-22)
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$(tip)" = "$old" ] && mirror_is_upstream'
  check 'ce_show yacer:kodi.source | grep -qx "kodi $(kodiid "$RM/cece.git" coreelec-22)"'
}

s_pin_sideways_backwards() {
  restore bumped
  local p1 p1b p0
  p1=$(sed -n 's/^PKG_VERSION="\(.*\)"/\1/p' "$R/wc/$KPKG/package.mk")
  p0=$(git -C "$RM/cexbmc.git" rev-parse aml)
  p1b=$(kodi_up pin1b aml "Video: unrelated upstream work, rewritten" xbmc/Video.cpp 35 "Video line 35 upstream" xbmc/Video.cpp 8 "Video generated with tools")
  git -C "$RM/cexbmc.git" branch -q -D pin1
  git -C "$RM/cexbmc.git" gc -q --prune=now
  check '! git -C "$RM/cexbmc.git" cat-file -e "$p1^{commit}" 2>/dev/null'
  ce_up "$p1b"
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream && stack_matches_names'
  check 'git -C "$RM/aoxbmc.git" cat-file -e "$p1^{commit}"'
  ce_up "$p0"
  sync_ci
  check '[ "$RESULT" = ok ] && mirror_is_upstream && stack_matches_names'
}

s_push_races() {
  restore boot
  local p1 th
  p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  plan_ci
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 h/" xbmc/Video.cpp && git commit -qa -m "Video: a human was faster" -m "'"$SOB"'"' >> "$LOG" 2>&1
  land_and_push "kodi: human"
  th=$(tip); refs > "$ROOT/before"
  push_ci
  check '[ "$PUSHRC" != 0 ] && refs | cmp -s - "$ROOT/before"'
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ] && [ "$(git -C "$RM/aoxbmc.git" rev-parse "$(tip)^1")" = "$th" ] && mirror_is_upstream'
  # the mirror moved by someone else between plan and push
  restore boot
  p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  plan_ci
  (cd "$R/wc" && git commit -q --allow-empty -m human && git push -q -f "file://$RM/aoce.git" HEAD:refs/heads/coreelec-22)
  local y0; y0=$(git -C "$RM/aoce.git" rev-parse yacer)
  push_ci
  check '[ "$PUSHRC" != 0 ] && [ "$(git -C "$RM/aoce.git" rev-parse yacer)" = "$y0" ]'
  check '[ "$(git -C "$RM/aoce.git" rev-parse coreelec-22)" = "$(git -C "$R/wc" rev-parse HEAD)" ]'
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ] && mirror_is_upstream'
  check '! git -C "$RM/aoce.git" rev-parse -q --verify refs/heads/yacer-mirror-next'
}

s_push_validation() {
  restore boot
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  plan_ci
  refs > "$ROOT/before"
  git -C "$ROOT/runner/tmp/xbmc" config core.sshCommand "touch $ROOT/pwned"
  push_ci
  check '[ "$PUSHRC" != 0 ] && grep -q "unexpected local config" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  git -C "$ROOT/runner/tmp/xbmc" config --unset core.sshCommand
  sed -i "s#^xb=.*#xb=$ROOT/elsewhere#" "$ROOT/runner/plan"
  push_ci
  check '[ "$PUSHRC" != 0 ] && grep -q "bad xb" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
}

s_hand_edits() {
  restore boot
  echo "extra" >> "$CY/$Y/0012-VideoPlayer-an-old-idea.patch.disabled"
  git -C "$CY" add -A; cy_push "edit an inactive patch"
  sync_ci
  check '[ "$RESULT" = ok ]'
  ce_show "yacer:$Y/0011-FileCache-a-stalled-source.patch" > "$CY/$Y/0099-FileCache-copy.patch.disabled"
  git -C "$CY" add -A; cy_push "keep an old copy"
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  sync_ci
  check '[ "$RESULT" = ok ] && active | grep -qx 0011-FileCache-a-stalled-source.patch'
  check 'allfiles | grep -qx 0099-FileCache-copy.patch.disabled'
  cy_update
  sed -i 's/^Short body\.$/Short body, edited./' "$CY/$Y/0010-ProcessInfo-publish-whether-the-codec-buffers.patch"
  git -C "$CY" add -A; cy_push "hand edit"
  refs > "$ROOT/before"; sync_ci
  check '[ "$RESULT" = fail ] && grep -q "edited by hand" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  git -C "$CY" revert --no-edit HEAD >/dev/null; git -C "$CY" push -q origin yacer
  git -C "$CY" mv "$Y/0099-FileCache-copy.patch.disabled" "$Y/0099-FileCache-copy.patch"
  cy_push "hand re-enable"
  sync_ci
  check '[ "$RESULT" = fail ] && grep -q "not in kodi.source" <<<"$SLOG"'
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = fail ]'
}

s_git_rm() {
  restore boot
  git -C "$CY" rm -q "$Y/1026-AMLCodec-busy-spin.patch"
  cy_push "kodi: delete busy-spin"
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = skip ]'
  sync_ci
  check '[ "$RESULT" = ok ] && ! allfiles | grep -q 1026 && ! ce_show yacer:kodi.source | grep -q busy-spin && stack_matches_names'
}

s_all_disabled() {
  restore boot
  local f
  for f in $(active); do git -C "$CY" mv "$Y/$f" "$Y/$f.disabled"; done
  cy_push "kodi: disable everything"
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = skip ]'
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ]'
  check '[ -z "$(active)" ] && ! ce_show yacer:kodi.source | grep -q ^patch'
  check 'git -C "$RM/aoxbmc.git" log -1 --format=%s "$(tip)^2" | grep -q "^yacer-base: "'
  guard_ci yacer "$RM/aoce.git" coreelec-22
  check '[ "$GUARD" = ok ]'
}

s_tip_moved() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "5s/.*/Video line 5 ours v2/" xbmc/Video.cpp && git commit -qa --fixup=HEAD~4' >> "$LOG" 2>&1
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  sync_ci
  check '[ "$RESULT" = ok ]'
  must_fail "tip moved.*abandon" yk start
  must_fail "tip moved.*abandon" yk land -m "kodi: late"
  yk abandon >> "$LOG" 2>&1
  check '[ -n "$(git -C "$XB" for-each-ref refs/yacer/abandoned)" ]'
  check '[ ! -d "$WT_" ]'
  must_fail "update local yacer" yk start
  cy_update
  yk start >> "$LOG" 2>&1
  check '[ -d "$WT_" ]'
}

s_pr_merged_with_changes() {
  restore boot
  local p5
  p5=$(kodi_up pin5 aml "FileCache: handle a stalled source (merged PR, reworked)" xbmc/FileCache.cpp 10 "FileCache line 10 merged differently")
  ce_up "$p5"
  sync_ci
  check '[ "$RESULT" = conflict ]'
  cy_update
  git -C "$CY" mv "$Y/0011-FileCache-a-stalled-source.patch" "$Y/0011-FileCache-a-stalled-source.patch.merged"
  cy_push "kodi: FileCache stall merged upstream"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ] && mirror_is_upstream'
  check 'allfiles | grep -qx 0011-FileCache-a-stalled-source.patch.merged'
  check '! ce_show yacer:kodi.source | grep -q FileCache'
}

s_config_isolation() {
  restore boot
  mkdir -p "$XDG_CONFIG_HOME/git"; echo '* -diff' > "$XDG_CONFIG_HOME/git/attributes"
  local p1; p1=$(kodi_up pin1 aml "Video: unrelated upstream work" xbmc/Video.cpp 35 "Video line 35 upstream")
  ce_up "$p1"
  sync_ci
  check '[ "$RESULT" = ok ] && [ "$PUSHRC" = 0 ]'
  check '! ce_show "yacer:$Y/0010-ProcessInfo-publish-whether-the-codec-buffers.patch" | grep -q "^Binary files"'
  cy_update
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 c/" xbmc/Video.cpp && git commit -qa -m "Video: under a hostile attributes file" -m "'"$SOB"'"' >> "$LOG" 2>&1
  land_and_push "kodi: attributes"
  rm -f "$XDG_CONFIG_HOME/git/attributes"
}

s_guard_local() {
  restore boot
  cy_update
  (cd "$CE" && git checkout -q origin/coreelec-22 && bash "$CY/scripts/yacer-kodi.sh" guard --local "$CY/kodi.source") >> "$LOG" 2>&1
  echo junk > "$CE/$K2/extra.patch"
  must_fail "modified locally" bash -c "cd '$CE' && bash '$CY/scripts/yacer-kodi.sh' guard --local '$CY/kodi.source'"
  rm "$CE/$K2/extra.patch"
  echo '# local' >> "$CE/$KPKG/package.mk"
  must_fail "modified locally" bash -c "cd '$CE' && bash '$CY/scripts/yacer-kodi.sh' guard --local '$CY/kodi.source'"
  git -C "$CE" checkout -q -- "$KPKG/package.mk"
  conflict_bump; git -C "$CE" fetch -q upstream
  must_fail "another CoreELEC kodi" bash -c "cd '$CE' && git checkout -q upstream/coreelec-22 && bash '$CY/scripts/yacer-kodi.sh' guard --local '$CY/kodi.source'"
  git -C "$CE" checkout -q coreelec-22
  cp "$SRC/scripts/yacer-local.sh" "$CY/scripts/"
  mkdir -p "$CY/tree-patches"
  (cd "$CE" && bash "$CY/scripts/yacer-local.sh" apply && bash "$CY/scripts/yacer-local.sh" revert) >> "$LOG" 2>&1
  yk patches >> "$LOG" 2>&1
  (cd "$CE" && YACER_KODI_PATCHES=$XB/.git/yacer-patches bash "$CY/scripts/yacer-local.sh" apply && bash "$CY/scripts/yacer-local.sh" revert) >> "$LOG" 2>&1
  must_fail "another CoreELEC kodi" bash -c "cd '$CE' && git checkout -q upstream/coreelec-22 && YACER_MIRROR=upstream/coreelec-22 bash '$CY/scripts/yacer-local.sh' apply"
  check '[ ! -f "$CE/.yacer-local-state" ]'
  git -C "$CE" checkout -q coreelec-22
}

s_screening() {
  local bad
  for bad in "../evil" "/etc/evil" ".git/config" "xbmc/.git/hooks/x"; do
    restore boot
    cd "$R/wc"
    printf -- '--- a/%s\n+++ b/%s\n@@ -0,0 +1 @@\n+x\n' "$bad" "$bad" > "$K2/kodi-07-bad.patch"
    git add -A; git commit -qm bad; git push -q origin coreelec-22; cd "$ROOT"
    refs > "$ROOT/before"; sync_ci
    check '[ "$RESULT" = fail ] && grep -q "unsafe path" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
  done
}

s_rej() {
  restore boot
  cd "$R/wc"
  printf -- '--- a/libdvd.txt\n+++ b/libdvd.txt\n@@ -20,3 +20,3 @@\n not there\n-nope\n+yes\n not there\n' > "$K2/kodi-07-stale.patch"
  git add -A; git commit -qm stale; git push -q origin coreelec-22; cd "$ROOT"
  refs > "$ROOT/before"; sync_ci
  check '[ "$RESULT" = fail ] && grep -q "does not apply" <<<"$SLOG" && refs | cmp -s - "$ROOT/before"'
}

s_unique_subjects() {
  restore boot
  yk start >> "$LOG" 2>&1
  wt bash -c 'sed -i "38s/.*/Video line 38 dup/" xbmc/Video.cpp && git commit -qam "ProcessInfo: publish whether the codec buffers" -m "'"$SOB"'"' >> "$LOG" 2>&1
  must_fail "unique" yk land -m "kodi: dup"
}

s_local_config() {
  restore boot
  local k
  for k in diff.noprefix rebase.autoSquash rerere.autoUpdate commit.cleanup log.showSignature; do
    git -C "$XB" config --local "$k" true
    must_fail "local settings" yk start
    git -C "$XB" config --local --unset "$k"
  done
  git -C "$XB" config --local --unset rerere.enabled
  must_fail "rerere" yk start
  git -C "$XB" config --local rerere.enabled true
  git -C "$XB" remote set-url origin "file://$RM/cexbmc.git"
  must_fail "remote 'origin'" yk start
}

s_workflows() {
  local sync=$SRC/.github/workflows/yacer-sync.yml build=$SRC/.github/workflows/yacer-build.yml steps
  steps=$(awk '/^      - (name|uses):/{step=$0} /secrets\./{print step}' "$sync" | sort -u)
  check '[ "$steps" = "      - name: Push" ]'
  check '[ "$(grep -c XBMC_DEPLOY_KEY "$sync")" -eq 1 ]'
  check 'grep -q "persist-credentials: false" "$sync"'
  check '! awk "/- name: Plan/{p=1} /- name: Push/{p=0} p" "$sync" | grep -q "env:"'
  check 'grep -q "environment: yacer-push" "$sync"'
  check 'grep -q "needs.guard.outputs.mirror" "$build" && grep -q "needs: guard" "$build"'
  check '! grep -rq "yacer-mirror-next" "$SRC/.github/workflows"'
  check 'grep -q "if: inputs.mirror == ..$" "$build" && grep -q -- "-f mirror=" "$build"'
  check 'grep -q "needs.guard.result == .skipped." "$build"'
  if command -v actionlint >/dev/null; then check 'actionlint "$sync" "$build"'; else say "actionlint not installed - skipped"; fi
  if command -v shellcheck >/dev/null; then check 'shellcheck -S warning "$SRC/scripts/yacer-kodi.sh" "$SRC/scripts/yacer-local.sh"'; else say "shellcheck not installed - skipped"; fi
}

ALL=(s_bootstrap s_clean_bump s_deploy_key s_conflict_bump s_held_mirror_fallback s_start_upstream s_hand_continue
     s_not_provable_hand_skip s_landed_then_conflict s_internal_error s_fail_no_fallback s_pending_land_then_disable s_fallback_needs_same_base s_reword_mid_rebase s_land_twice s_fully_merged
     s_partial_merge s_partly_upstream s_disable_conflicting s_enable_after_bumps s_enable_paths s_insert_consecutive s_reorder
     s_reword_and_edit s_subject_chars s_replay_restores s_attribution s_missed_bumps s_coreelec_only
     s_pin_sideways_backwards s_push_races s_push_validation s_hand_edits s_git_rm s_all_disabled s_tip_moved
     s_pr_merged_with_changes s_config_isolation s_guard_local s_screening s_rej s_unique_subjects s_local_config
     s_workflows)
[ $# -eq 0 ] || ALL=("$@")

pass=0; failed=()
for s in "${ALL[@]}"; do
  : > "$LOG"
  set +e
  ( set -e; cd "$ROOT"; "$s" )
  rc=$?
  set -e
  if [ $rc -eq 0 ]; then
    echo "PASS $s"; pass=$((pass + 1))
  else
    echo "FAIL $s"; failed+=("$s")
    tail -n 60 "$LOG" | sed 's/^/    /'
    [ "$s" != s_bootstrap ] || break
  fi
done
echo "$pass passed, ${#failed[@]} failed${failed:+: ${failed[*]}}"
[ ${#failed[@]} -eq 0 ]
