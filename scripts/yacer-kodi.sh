#!/usr/bin/env bash
#
# Our kodi patches as a git branch. allolive/xbmc yacer-kodi holds them as
# commits on top of the pinned kodi and CoreELEC's own kodi patches; the .patch
# files on yacer are regenerated from it, never edited by hand.
#
#   plan, push <plan>        CI sync: rebase onto upstream, then push the result
#   guard [--local [file]]   refuse a build whose patches do not fit the mirror
#   import [<ce> [<yacer>]]  create yacer-kodi from the .patch files (once)
#   start [--upstream]       rebase locally; run again after resolving
#   enable <file>            put a .disabled/.merged patch back on the stack
#   land -m <message>        record the local stack as one commit on yacer-kodi
#   patches                  write the regenerated files for a local build
#   abandon                  drop the local work, keeping it as a ref
#
# The branch tip T always has two parents, the old tip and the stack R, with
# T^{tree} == R^{tree}. The stack is: pin -> yacer-base commit -> one commit per
# active patch, in build order.
set -euo pipefail
shopt -s inherit_errexit nullglob
[ -z "${YACER_TRACE:-}" ] || set -x

ME=allolive
MAIL=160342668+allolive@users.noreply.github.com
SOB="Signed-off-by: $ME <$MAIL>"
ATTR='co-authored-by: claude|anthropic|claude-session|generated with'
PARTLY_HINT="resolve by keeping upstream's version where they overlap; what remains is the part they did not take - if nothing remains, mark the file .patch.merged"
Y=overlay/projects/Amlogic-ce/patches-yacer/kodi
KPKG=projects/Amlogic-ce/packages/mediacenter/kodi
K1=$KPKG/patches
K2=projects/Amlogic-ce/patches/kodi
K3=projects/Amlogic-ce/devices/Amlogic-no/patches/kodi
YR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
DOC=$HOME/Documents/coreelec
XB=${YACER_XB:-$DOC/xbmc}
CE=${YACER_CE:-$DOC/CoreELEC}
CY=${YACER_CY:-$DOC/CoreELEC-yacer}
URL_XBMC=${YACER_URL_XBMC:-https://github.com/allolive/xbmc}
URL_CXBMC=${YACER_URL_CXBMC:-https://github.com/CoreELEC/xbmc}
URL_CE=${YACER_URL_CE:-https://github.com/allolive/CoreELEC}
URL_UP=${YACER_URL_UP:-https://github.com/CoreELEC/CoreELEC}
PUSH_XBMC=${YACER_PUSH_XBMC:-git@github.com:allolive/xbmc.git}
STAGE=refs/heads/yacer-mirror-next
KNOWN_HOST='github.com ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIOMqqnkVzrm0SdG6UOoqKLsabgH5C9okWi0dh2l9GKJl'

die() { echo "yacer-kodi: $*" >&2; exit 1; }
say() { echo "$*" >&2; }

export GIT_CONFIG_GLOBAL=/dev/null GIT_CONFIG_NOSYSTEM=1 GIT_EDITOR=true LC_ALL=C
export GIT_AUTHOR_NAME=$ME GIT_AUTHOR_EMAIL=$MAIL GIT_COMMITTER_NAME=$ME GIT_COMMITTER_EMAIL=$MAIL
unset GIT_DIR GIT_WORK_TREE GIT_INDEX_FILE GIT_CONFIG_COUNT GIT_CONFIG_PARAMETERS GIT_SSH_COMMAND
CFGN=0
force_config() {
  export "GIT_CONFIG_KEY_$CFGN=$1" "GIT_CONFIG_VALUE_$CFGN=$2"
  CFGN=$((CFGN + 1)); export GIT_CONFIG_COUNT=$CFGN
}
force_config core.attributesFile /dev/null
force_config core.excludesFile /dev/null
force_config core.quotePath true
force_config rerere.autoUpdate false
force_config log.showSignature false
force_config commit.gpgSign false
force_config rebase.autoSquash false
force_config rebase.updateRefs false
force_config rebase.missingCommitsCheck ignore
force_config merge.conflictStyle zdiff3
CIMODE=
if [ -n "${YACER_CI:-}" ] || [ "${GITHUB_ACTIONS:-}" = true ]; then
  CIMODE=1
  force_config core.hooksPath /dev/null
fi

read -r GV1 GV2 _ < <(git version | sed -n 's/^git version \([0-9]*\)\.\([0-9]*\).*/\1 \2 x/p')
[ "${GV1:-0}" -gt 2 ] || { [ "${GV1:-0}" -eq 2 ] && [ "${GV2:-0}" -ge 45 ]; } || die "git 2.45 or newer is needed"

KEYF=""
T=$(mktemp -d)
trap 'rm -rf "$T"; [ -z "$KEYF" ] || rm -f "$KEYF"' EXIT

report() { echo "$*" >&2; [ -z "${GITHUB_STEP_SUMMARY:-}" ] || echo "$*" >> "$GITHUB_STEP_SUMMARY"; }
# log_block <title> <file>: untrusted text to the log, workflow commands off in CI
log_block() {
  local tok=""
  [ -z "$CIMODE" ] || { tok=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n'); echo "::stop-commands::$tok" >&2; }
  echo "$1" >&2; sed 's/^/    /' "$2" >&2
  [ -z "$tok" ] || echo "::$tok::" >&2
}
# report_block <title> <file>: log_block, plus a bounded copy in the job summary
report_block() {
  log_block "$1" "$2"
  [ -n "${GITHUB_STEP_SUMMARY:-}" ] || return 0
  {
    echo "$1"; echo '```'; head -c 20000 "$2"
    [ "$(wc -c < "$2")" -le 20000 ] || printf '\n... truncated, see the log'
    echo; echo '```'
  } >> "$GITHUB_STEP_SUMMARY"
}

gitpath() { git rev-parse --path-format=absolute --git-path "$1"; }
busy()    { [ -d "$(gitpath rebase-merge)" ] || [ -d "$(gitpath rebase-apply)" ]; }
subj()    { git log -1 --format=%s "$1"; }
sha_ok()  { [[ $1 =~ ^[0-9a-f]{40}([0-9a-f]{24})?$ ]]; }

# upstream_commits <old pin> <new pin> <file>...: $T/uplog, the new upstream kodi
# commits on those files. Upstream rebases its kodi branch, so the list is capped
# and says so: some entries only look new.
upstream_commits() {
  local old=$1 new=$2 n
  shift 2
  : > "$T/uplog"
  [ $# -gt 0 ] || return 0
  git log --oneline --cherry-pick --right-only --no-merges "$old...$new" -- "$@" > "$T/uplog.all"
  n=$(wc -l < "$T/uplog.all")
  [ "$n" -gt 0 ] || return 0
  head -n 10 "$T/uplog.all" > "$T/uplog"
  [ "$n" -le 10 ] || echo "... and $((n - 10)) more" >> "$T/uplog"
  echo "(upstream rebases its kodi branch, so some of these may only look new)" >> "$T/uplog"
}

# reverse_applies <tree-ish> <patch file>: the change is already in that tree
reverse_applies() {
  GIT_INDEX_FILE=$T/ridx git read-tree "$1" \
    && GIT_INDEX_FILE=$T/ridx git apply -R --check --cached < "$2" >/dev/null 2>&1
}

# ------------------------------------------------------------------ checks
nurl() { local u=${1%/}; u=${u%.git}; u=${u#https://github.com/}; u=${u#ssh://git@github.com/}; u=${u#git@github.com:}; echo "${u,,}"; }
need_remote() {
  [ "$(nurl "$(git -C "$1" remote get-url "$2" 2>/dev/null || true)")" = "$(nurl "$3")" ] \
    || die "$1: remote '$2' must be $3"
}
need_config() {
  local bad
  bad=$(git -C "$1" config --local --get-regexp \
    '^(diff|format|am|apply|commit|rebase|log|sequence|patchid|gpg|i18n)\.|^core\.(autocrlf|whitespace|eol|safecrlf|abbrev|commentchar|commentstring)$|^merge\.renormalize$|^rerere\.autoupdate$' || true)
  [ -z "$bad" ] || die "$1 has local settings that change patches - remove them: $bad"
}
refuse_home() {
  local d
  [ "${GITHUB_ACTIONS:-}" != true ] || return 0
  for d in "$@"; do
    case "$(cd "$d" && pwd -P)/" in "$DOC"/*) die "plan and push run on a CI runner, not in $DOC" ;; esac
  done
}

local_env() {
  [ -z "$CIMODE" ] || die "this subcommand is local only"
  local d
  for d in "$XB" "$CE" "$CY"; do [ -d "$d" ] || die "no $d"; done
  need_remote "$XB" origin "$URL_XBMC"; need_remote "$XB" coreelec "$URL_CXBMC"
  need_remote "$CE" origin "$URL_CE"; need_remote "$CE" upstream "$URL_UP"
  need_config "$XB"; need_config "$CE"
  [ "$(git -C "$XB" config --local --get rerere.enabled || true)" = true ] \
    || die "set once: git -C $XB config --local rerere.enabled true"
  WT=$(git -C "$XB" rev-parse --path-format=absolute --git-common-dir)/yacer-work
  OUT=$(git -C "$XB" rev-parse --path-format=absolute --git-common-dir)/yacer-patches
  git -C "$XB" worktree prune
}

fetch_all() {
  git -C "$XB" fetch -q origin +refs/heads/yacer-kodi:refs/remotes/origin/yacer-kodi
  git -C "$CE" fetch -q upstream +refs/heads/coreelec-22:refs/remotes/upstream/coreelec-22
  git -C "$CE" fetch -q origin +refs/heads/coreelec-22:refs/remotes/origin/coreelec-22 +refs/heads/yacer:refs/remotes/origin/yacer
}

# a land that was never pushed must not be built over or overwritten
check_local_branch() {
  local l
  l=$(git -C "$XB" rev-parse -q --verify refs/heads/yacer-kodi || true)
  [ -n "$l" ] || return 0
  [ "$l" = "$(git -C "$XB" rev-parse refs/remotes/origin/yacer-kodi)" ] && return 0
  git -C "$XB" merge-base --is-ancestor "$l" refs/remotes/origin/yacer-kodi \
    || die "local yacer-kodi ${l:0:12} is not on origin: push it (git -C $XB push origin yacer-kodi, confirmed) or delete it"
}

# --------------------------------------------------------------- helpers
# kodiid <repo> <CoreELEC commit>: tree ids of everything that decides the base
kodiid() {
  local out="" d id
  git -C "$1" rev-parse -q --verify "$2^{commit}" >/dev/null || die "no CoreELEC commit $2"
  for d in "$KPKG" "$K2" "$K3"; do
    id=-
    if git -C "$1" cat-file -e "$2:$d" 2>/dev/null; then id=$(git -C "$1" rev-parse "$2:$d"); fi
    out+="${out:+ }$id"
  done
  echo "$out"
}
kodiline() { git log -1 --format=%B "$1" | sed -n 's/^kodi //p'; }

# pin_of <repo> <CoreELEC commit>
pin_of() {
  local pin
  git -C "$1" cat-file -e "$2:$KPKG/package.mk" 2>/dev/null || die "no $KPKG/package.mk in ${2:0:12}"
  git -C "$1" show "$2:$KPKG/package.mk" > "$T/package.mk"
  pin=$(sed -n '/^PKG_VERSION="[0-9a-f]\{40\}"/{s/^PKG_VERSION="\([0-9a-f]\{40\}\)".*/\1/p;q}' "$T/package.mk")
  [ -n "$pin" ] || die "no PKG_VERSION in ${2:0:12}"
  echo "$pin"
}

# obof <stack>: the yacer-base commit of a stack
obof() {
  local ob s
  ob=$(git rev-list --first-parent -1 --grep='^yacer-base: ' "$1")
  [ -n "$ob" ] || die "no yacer-base commit under ${1:0:12}"
  s=$(subj "$ob")
  case "$s" in "yacer-base: CoreELEC kodi patches for "*) ;; *) die "bad base subject: $s" ;; esac
  [ "$(git rev-parse "$ob^")" = "${s##* }" ] || die "base ${ob:0:12} does not sit on the pin it names"
  echo "$ob"
}

stem() { local f=${1%.merged}; f=${f%.disabled}; echo "${f%.patch}"; }
key()  { local s; s=$(stem "$1"); echo "${s%%-*}"; }
mi_subject() { git mailinfo -b "$T/mi.msg" "$T/mi.diff" < "$1" > "$T/mi.info" 2>/dev/null || true; sed -n 's/^Subject: //p' "$T/mi.info"; }

# screen <patch> <label>: refuse absolute paths and .. or .git components
screen() {
  local p
  sed -nE -e 's#^diff --git a/(.*) b/(.*)$#\1\n\2#p' \
           -e 's#^(---|\+\+\+) (a/|b/)?([^\t]*).*$#\3#p' \
           -e 's#^(rename|copy) (from|to) (.*)$#\3#p' "$1" > "$T/paths"
  while IFS= read -r p; do
    case "$p" in /dev/null|'') continue ;; esac
    case "/$p/" in //*|*/../*|*/.git/*) die "unsafe path '$p' in $2" ;; esac
  done < "$T/paths"
}

# apply_unpack <patch>: scripts/unpack's command, in the current worktree
apply_unpack() {
  if grep -qE '^GIT binary patch$|^rename from|^rename to' "$1"; then
    git apply --directory=. -p1 --verbose --whitespace=nowarn --unsafe-paths < "$1" > "$T/apply.log" 2>&1
  else
    setsid -w patch -p1 < "$1" > "$T/apply.log" 2>&1
  fi || { sed 's/^/    /' "$T/apply.log" >&2; return 1; }
}

# patch_names <repo> <commit> <dir> > list: *.patch names in a tree dir, C order
patch_names() {
  git -C "$1" cat-file -e "$2:$3" 2>/dev/null || return 0
  git -C "$1" ls-tree --name-only "$2:$3" > "$T/pn.all"
  { grep '\.patch$' "$T/pn.all" || true; } > "$T/pn.list"
  sort "$T/pn.list"
}

# ------------------------------------------------------------------- base
# base <CoreELEC repo> <commit> [old base]: in the kodi worktree, commit the pin
# with CoreELEC's kodi patches applied. Sets PIN and NB; NB is the old base when
# it sits on the same pin with the same tree.
base() {
  local cr=$1 c ob=${3:-} d f id
  c=$(git -C "$cr" rev-parse -q --verify "$2^{commit}") || die "no CoreELEC commit $2"
  PIN=$(pin_of "$cr" "$c")
  for d in "$K1" "$K2" "$K3"; do
    git -C "$cr" cat-file -e "$c:$d" 2>/dev/null || continue
    [ -z "$(git -C "$cr" ls-tree -d "$c:$d")" ] || die "$d in ${c:0:12} has subdirectories"
  done
  git cat-file -e "$PIN^{commit}" 2>/dev/null || git fetch -q coreelec "$PIN" || die "cannot fetch kodi $PIN"
  git checkout -q -f --detach "$PIN"
  git clean -fdxq
  id=$(kodiid "$cr" "$c")
  for d in "$K1" "$K2" "$K3"; do
    patch_names "$cr" "$c" "$d" > "$T/ce.list"
    while IFS= read -r f; do
      git -C "$cr" show "$c:$d/$f" > "$T/ce.patch"
      screen "$T/ce.patch" "$d/$f"
      apply_unpack "$T/ce.patch" || die "CoreELEC $d/$f does not apply to kodi ${PIN:0:12}"
    done < "$T/ce.list"
  done
  [ -z "$(git ls-files -o -- '*.rej')" ] || die "CoreELEC kodi patches left .rej files"
  git ls-files -o -z -- '*.orig' | xargs -0r rm -f
  git add -A -f
  git commit -q --allow-empty -m "yacer-base: CoreELEC kodi patches for $PIN" -m "CoreELEC $c" -m "kodi $id"
  NB=$(git rev-parse HEAD)
  if [ -n "$ob" ] && [ "$(git rev-parse "$ob^")" = "$PIN" ]; then
    if [ "$(git rev-parse "$ob^{tree}")" = "$(git rev-parse "$NB^{tree}")" ]; then
      NB=$ob
    elif [ "$(kodiline "$ob")" = "$id" ]; then
      die "base differs between patch versions: ${ob:0:12} came from the same CoreELEC files with a different result"
    fi
  fi
}

# ----------------------------------------------------------------- intent
# load_source <file>: S (branch), K (kodi), $T/P ("blob name subject" per active patch)
load_source() {
  [ -s "$1" ] || die "no kodi.source"
  S=$(sed -n 's/^branch //p' "$1"); K=$(sed -n 's/^kodi //p' "$1")
  sed -n 's/^patch //p' "$1" > "$T/P"
  [ -n "$S" ] && [ -n "$K" ] || die "kodi.source is incomplete"
}
# listing <repo> <commit>: $T/D ("name blob" per file in $Y)
listing() {
  local line meta path
  : > "$T/D"
  git -C "$1" ls-tree -z "$2" "$Y/" > "$T/lstree"
  while IFS= read -r -d '' line; do
    meta=${line%%$'\t'*}; path=${line#*$'\t'}
    case "$meta" in *" blob "*) ;; *) continue ;; esac
    case "${path##*/}" in *[[:space:]]*) die "patch file names must not contain spaces: ${path##*/}" ;; esac
    echo "${path##*/} ${meta##* }" >> "$T/D"
  done < "$T/lstree"
}
# name_for_subject <subject>: the kodi.source file name of an active patch
name_for_subject() {
  local b n s
  while read -r b n s; do [ "$s" != "$1" ] || { echo "$n"; return 0; }; done < "$T/P"
}
# intent: $T/drops = subjects of kodi.source patches no longer active on yacer.
# Dies on hand edits and on active files the sync did not write.
intent() {
  local blob name subject bad="" inactive=0
  local -A dblob=() pname=()
  while read -r name blob; do dblob[$name]=$blob; done < "$T/D"
  : > "$T/drops"
  while read -r blob name subject; do
    pname[$name]=1
    if [ "${dblob[$name]:-}" = "$blob" ]; then :
    elif [ -n "${dblob[$name]:-}" ]; then bad+="  $name edited by hand - port it to yacer-kodi"$'\n'
    else echo "$subject" >> "$T/drops"; fi
  done < "$T/P"
  for name in "${!dblob[@]}"; do
    case "$name" in
      *.patch) [ -n "${pname[$name]:-}" ] \
        || bad+="  $name is not in kodi.source - add, reorder or re-enable through the branch (yacer-kodi.sh enable)"$'\n' ;;
      *) inactive=$((inactive + 1)) ;;
    esac
  done
  [ -z "$bad" ] || die "kodi patches changed on yacer outside the branch:"$'\n'"$bad"
  [ ! -s "$T/drops" ] || log_block "dropping (removed on yacer):" "$T/drops"
  [ "$inactive" -eq 0 ] || say "note: $inactive inactive patch file(s) on yacer are ignored"
}

# ------------------------------------------------------------ todo + loop
# commit_with_subject <file of "sha subject" lines> <subject>
commit_with_subject() {
  local c t
  while read -r c t; do [ "$t" != "$2" ] || { echo "$c"; return 0; }; done < "$1"
}

# check_drops <old base> <stack>: a drop that matches no commit is only a no-op
# when the patch is not in the stack kodi.source was written for either. A land
# that reworded it moves the subject, and keeping the commit would put the patch
# back while its file stays disabled.
check_drops() {
  local s sc c p ob
  [ -s "$T/drops" ] || return 0
  git rev-parse -q --verify "$S^2^{commit}" >/dev/null 2>&1 || return 0
  ob=$(obof "$S^2")
  git log --format='%H %s' "$ob..$S^2" > "$T/oldstack"
  while IFS= read -r s; do
    [ -z "$(commit_with_subject "$T/stack" "$s")" ] || continue
    sc=$(commit_with_subject "$T/oldstack" "$s")
    [ -n "$sc" ] || continue
    p=$(pid "$sc")
    while read -r c _; do
      [ "$(pid "$c")" = "$p" ] || continue
      needs_human "'$s' was reworded by a land the sync has not written yet - wait for the sync, then disable the new file"
    done < "$T/stack"
  done < "$T/drops"
}

# mktodo <old base> <stack>: $TODO, every stack commit not dropped
mktodo() {
  local c s
  [ -z "$(git rev-list --merges "$1..$2")" ] || die "the stack has merge commits"
  git log --reverse --format='%H %s' "$1..$2" > "$T/stack"
  check_drops "$1" "$2"
  echo noop > "$TODO"
  while read -r c s; do
    grep -Fxq -- "$s" "$T/drops" || echo "pick $c $s" >> "$TODO"
  done < "$T/stack"
}

# needs_human <message>: a conflict or change only a person can judge. CI aborts
# and records it ($NEEDS -> result=conflict); locally the state stays for the human.
needs_human() {
  report "$*"
  if [ -n "$CIMODE" ]; then
    echo "$*" >> "$NEEDS"
    git rebase --abort >/dev/null 2>&1 || true
    git am --abort >/dev/null 2>&1 || true
  fi
  exit 1
}

report_conflict() {
  local s name
  local -a files
  s=$(subj REBASE_HEAD); name=$(name_for_subject "$s")
  git diff -z --name-only --diff-filter=U > "$T/unmerged"
  mapfile -d '' -t files < "$T/unmerged"
  {
    echo "patch: $s"
    echo "file: ${name:-(not on yacer yet)}"
    echo "conflicting files:"
    printf '  %s\n' "${files[@]}"
    echo "new upstream kodi commits on them (${OLDPIN:0:12} -> ${PIN:0:12}):"
    upstream_commits "$OLDPIN" "$PIN" ${files[@]+"${files[@]}"}
    sed 's/^/  /' "$T/uplog"
    [ ! -s "$T/uplog" ] || echo "$PARTLY_HINT"
  } > "$T/report"
  report_block "### kodi patch does not apply" "$T/report"
  git -C "$CEREPO" diff --stat "$MIR" "$TGT" -- "$KPKG/package.mk" "$K1" "$K2" "$K3" > "$T/cestat"
  git -C "$CEREPO" diff "$MIR" "$TGT" -- "$K1" "$K2" "$K3" > "$T/cediff"
  [ ! -s "$T/cestat" ] || report_block "CoreELEC kodi changes ${MIR:0:12} -> ${TGT:0:12}:" "$T/cestat"
  [ ! -s "$T/cediff" ] || log_block "CoreELEC kodi patch diff:" "$T/cediff"
}

# local only (enable): a stopped git am
prove_am_empty() {
  if reverse_applies HEAD "$1/patch"; then
    say "already upstream: $(sed -n 's/^Subject: //p' "$1/info" 2>/dev/null) - leave it .merged"
    git am --skip
  else
    git am --abort
    die "the patch does not apply and has no 3-way base: apply it by hand (patch -p1, commit -s), then start"
  fi
}

rebase_onto() {
  local rc=0
  GIT_SEQUENCE_EDITOR="cp $(printf %q "$TODO")" git rebase -q -i --empty=stop \
    --reapply-cherry-picks --onto "$1" "$2" yacer-kodi-work >&2 || rc=$?
  [ "$rc" -eq 0 ] || [ -d "$(gitpath rebase-merge)" ] || die "rebase refused to start"
}

loop() {
  local rb ra last="" now i
  rb=$(gitpath rebase-merge); ra=$(gitpath rebase-apply)
  for i in $(seq 1 500); do
    if [ -d "$ra" ]; then
      [ -z "$(git diff --name-only --diff-filter=U)" ] || needs_human "resolve the conflicts, git add, then run start again"
      now="am $(cat "$ra/next")"; [ "$now" != "$last" ] || die "no progress in git am"; last=$now
      if git diff --cached --quiet; then prove_am_empty "$ra"; else git am --continue >&2 || true; fi
      continue
    fi
    [ -d "$rb" ] || break
    now="$(git rev-parse -q --verify REBASE_HEAD || true) $( { cat "$rb/done" 2>/dev/null || true; } | wc -l)"
    [ "$now" != "$last" ] || die "no progress in the rebase"; last=$now
    if [ -n "$(git diff --name-only --diff-filter=U)" ]; then
      report_conflict
      { git rerere status 2>/dev/null || true; } | sort > "$T/rr.known"
      { git rerere remaining 2>/dev/null || true; } | sort > "$T/rr.left"
      [ -z "$(comm -23 "$T/rr.known" "$T/rr.left")" ] \
        || say "rerere replayed an old resolution in some of these: review it, git add, then start"
      needs_human "resolve the conflicts in real code, git add, then run start again"
    elif git rev-parse -q --verify REBASE_HEAD >/dev/null && git diff --cached --quiet; then
      git diff REBASE_HEAD^ REBASE_HEAD > "$T/rh.diff"
      git apply -R --check --cached < "$T/rh.diff" >/dev/null 2>&1 \
        || needs_human "empty, but not provably upstream: $(subj REBASE_HEAD) - mark its file .merged on yacer, git rebase --skip, start"
      subj REBASE_HEAD >> "$SKIPS"
      say "already upstream: $(subj REBASE_HEAD)"
      git rebase --skip >&2 || true
    else
      git rebase --continue >&2 || true
    fi
  done
  if [ -d "$rb" ] || [ -d "$ra" ]; then die "loop limit"; fi
}

# check_subjects <from> <to>: unique, and unchanged by a trip through a patch file
check_subjects() {
  local d s
  git log --format=%s "$1..$2" > "$T/subjects"
  d=$(sort "$T/subjects" | uniq -d)
  [ -z "$d" ] || die "patch subjects must be unique: $d"
  while IFS= read -r s; do
    [ -n "$s" ] || die "a patch commit has an empty subject"
    [ "$(printf '%s' "$s" | tr -s ' \t' ' ' | sed 's/^ //; s/ $//')" = "$s" ] \
      || die "subject '$s' has repeated whitespace, which a patch file does not keep - reword it"
  done < "$T/subjects"
}

# post_asserts <new base>: the rebased stack is the todo, minus what was proven
# upstream, dropped on yacer, or skipped by hand with its change already there
post_asserts() {
  local c s p g i=0
  local -a gotl wantl
  git log --reverse --format='%H %s' "$1..HEAD" > "$T/gotc"
  cut -d' ' -f2- "$T/gotc" > "$T/got"
  : > "$T/want"
  while read -r _ c s; do
    if grep -Fxq -- "$s" "$T/got"; then echo "$s" >> "$T/want"; continue; fi
    if grep -Fxq -- "$s" "$SKIPS" || grep -Fxq -- "$s" "$T/drops"; then continue; fi
    p=$(pid "$c"); g=""
    while read -r n _; do [ "$(pid "$n")" != "$p" ] || { g=$n; break; }; done < "$T/gotc"
    [ -z "$g" ] || { say "reworded or split by hand: $s"; continue; }
    git diff "$c^" "$c" > "$T/pa.diff"
    if reverse_applies "$1" "$T/pa.diff" || reverse_applies HEAD "$T/pa.diff"; then
      say "skipped by hand, already upstream: $s"
      continue
    fi
    die "'$s' is missing from the rebased stack: if you skipped it, mark its file .merged or .disabled on yacer first"
  done < <(sed -n '/^pick /p' "$TODO")
  [ -z "$(git rev-list --merges "$1..HEAD")" ] || die "the rebased stack has merge commits"
  mapfile -t gotl < "$T/got"; mapfile -t wantl < "$T/want"
  for s in ${wantl[@]+"${wantl[@]}"}; do
    while [ $i -lt ${#gotl[@]} ] && [ "${gotl[i]}" != "$s" ]; do i=$((i + 1)); done
    [ $i -lt ${#gotl[@]} ] \
      || die "the stack no longer matches the todo: review, then yacer-kodi.sh abandon and start again (rerere replays your resolutions)"
    i=$((i + 1))
  done
  check_subjects "$1" HEAD
}

# shrank <old commit> <new commit>: the new change is a strict subset of the old
# one - upstream took part of the patch and the rest was kept
shrank() {
  git show --format= -U0 "$1" | { grep -E '^[-+]' || true; } | { grep -vE '^(\+\+\+|---) ' || true; } | sort -u > "$T/sh.old"
  git show --format= -U0 "$2" | { grep -E '^[-+]' || true; } | { grep -vE '^(\+\+\+|---) ' || true; } | sort -u > "$T/sh.new"
  [ -z "$(comm -13 "$T/sh.old" "$T/sh.new")" ] && [ -n "$(comm -23 "$T/sh.old" "$T/sh.new")" ]
}

# partly_upstream <old base> <old stack> <new base> <new stack>: add or refresh a
# Partly-upstream trailer on every commit whose change shrank in the rebase, so the
# regenerated patch says which upstream commits took the rest. Prints the new stack
# tip (the old one when there is nothing to record).
partly_upstream() {
  local c s o n parent=$3 an ae ad shas
  local -A trailer=()
  local -a files
  git log --format='%H %s' "$1..$2" > "$T/pu.old"
  git log --reverse --format=%H "$3..$4" > "$T/pu.new"
  while read -r c; do
    s=$(subj "$c")
    o=$(commit_with_subject "$T/pu.old" "$s")
    [ -n "$o" ] || continue
    shrank "$o" "$c" || continue
    git diff -z --name-only "$c^" "$c" > "$T/pu.files"
    mapfile -d '' -t files < "$T/pu.files"
    upstream_commits "$(git rev-parse "$1^")" "$(git rev-parse "$3^")" ${files[@]+"${files[@]}"}
    shas=$(head -n 5 "$T/uplog" | cut -d' ' -f1 | { grep -E '^[0-9a-f]{7,}$' || true; } | paste -sd' ')
    [ -n "$shas" ] || continue
    trailer[$s]=$shas
  done < "$T/pu.new"
  [ ${#trailer[@]} -gt 0 ] || { echo "$4"; return 0; }
  while read -r c; do
    s=$(subj "$c")
    git log -1 --format=%B "$c" > "$T/pu.msg"
    if [ -n "${trailer[$s]:-}" ]; then
      git interpret-trailers --if-exists replace --trailer "Partly-upstream: ${trailer[$s]}" "$T/pu.msg" > "$T/pu.msg2"
      mv "$T/pu.msg2" "$T/pu.msg"
      say "recorded Partly-upstream ${trailer[$s]} on '$s'"
    fi
    IFS=$'\t' read -r an ae ad < <(git log -1 --format='%an%x09%ae%x09%aI' "$c")
    n=$(GIT_AUTHOR_NAME=$an GIT_AUTHOR_EMAIL=$ae GIT_AUTHOR_DATE=$ad git commit-tree "$c^{tree}" -p "$parent" -F "$T/pu.msg")
    parent=$n
  done < "$T/pu.new"
  echo "$parent"
}

# ----------------------------------------------------------- names/regen
# names <dir>: which file each stack commit is written to (design §3 names).
#   in:  $T/commits (stack order), <dir> with the yacer patch files
#   out: CM[i] commit, MATCH[i] file it comes from or "", NAME[i] file to write,
#        LEFT[] active files no commit claims
names() { names_load "$1"; names_match "$1"; names_keep; names_allocate; }

names_load() {
  local f
  declare -gA FSUB=() FKEY=()
  FILES=(); NAME=(); MATCH=(); WANT=(); LEFT=()
  for f in "$1"/*.patch "$1"/*.patch.disabled "$1"/*.patch.merged; do
    f=${f##*/}; FILES+=("$f"); FSUB[$f]=$(mi_subject "$1/$f"); FKEY[$f]=$(key "$f")
  done
  mapfile -t FILES < <(printf '%s\n' "${FILES[@]}" | sed '/^$/d' | sort)
  mapfile -t CM < "$T/commits"
}

# step 1 (same subject: the active file, else the greatest inactive one) and
# step 2 (a reworded commit takes the unclaimed active file with its patch-id)
names_match() {
  local dir=$1 i j f s w pc po
  local -a act
  local -A used=()
  for ((i = 0; i < ${#CM[@]}; i++)); do
    s=$(subj "${CM[i]}"); act=(); w=""
    for f in "${FILES[@]}"; do
      [ "${FSUB[$f]}" = "$s" ] || continue
      case "$f" in *.patch) act+=("$f") ;; *) w=$f ;; esac
    done
    [ ${#act[@]} -le 1 ] || die "more than one active patch is titled '$s'"
    MATCH[i]=${act[0]:-$w}; WANT[i]=""
    [ -z "${MATCH[i]}" ] || used[${MATCH[i]}]=1
  done
  for f in "${FILES[@]}"; do
    case "$f" in *.patch) [ -n "${used[$f]:-}" ] || LEFT+=("$f") ;; esac
  done
  for ((i = 0; i < ${#CM[@]}; i++)); do
    if [ -n "${MATCH[i]}" ]; then WANT[i]=$(stem "${MATCH[i]}").patch; continue; fi
    [ ${#LEFT[@]} -gt 0 ] || continue
    git show "${CM[i]}" > "$T/c.show"
    pc=$(git patch-id --stable < "$T/c.show" | cut -d' ' -f1)
    for j in "${!LEFT[@]}"; do
      po=$(git patch-id --stable < "$dir/${LEFT[j]}" | cut -d' ' -f1)
      if [ -n "$po" ] && [ "$po" = "$pc" ]; then
        MATCH[i]=${LEFT[j]}; WANT[i]=${FKEY[${LEFT[j]}]}-$(git log -1 --format=%f "${CM[i]}").patch
        unset 'LEFT[j]'; LEFT=("${LEFT[@]}"); break
      fi
    done
  done
}

# step 3: keep a wanted name while it still sorts after the previous kept one
names_keep() {
  local i f w prev="" clash
  for ((i = 0; i < ${#CM[@]}; i++)); do
    NAME[i]=""; w=${WANT[i]}
    [ -n "$w" ] && [[ "$w" > "$prev" ]] || continue
    clash=0
    for f in "${FILES[@]}"; do [ "$f" != "$w" ] || [ "$f" = "${MATCH[i]}" ] || clash=1; done
    [ $clash -eq 0 ] || continue
    NAME[i]=$w; prev=$w
  done
}

# step 4: a free key between the neighbours: p+1, then p1..p9, then p01..p99
names_allocate() {
  local i j k f F lo hi p slug clash
  local -a cands
  local -A taken=()
  for ((i = 0; i < ${#CM[@]}; i++)); do [ -z "${NAME[i]}" ] || taken[$(key "${NAME[i]}")]=1; done
  for ((i = 0; i < ${#CM[@]}; i++)); do
    [ -z "${NAME[i]}" ] || continue
    lo=""; [ "$i" -eq 0 ] || lo=${NAME[i-1]}
    hi=""
    for ((j = i + 1; j < ${#CM[@]}; j++)); do [ -z "${NAME[j]}" ] || { hi=${NAME[j]}; break; }; done
    p=0000; [ -z "$lo" ] || p=$(key "$lo")
    slug=$(git log -1 --format=%f "${CM[i]}")
    cands=()
    if [[ $p =~ ^[0-9]+$ ]]; then
      k=$(printf '%0*d' ${#p} $((10#$p + 1))); [ ${#k} -gt ${#p} ] || cands+=("$k")
    fi
    for j in 1 2 3 4 5 6 7 8 9; do cands+=("$p$j"); done
    for j in $(seq -w 1 99); do cands+=("$p$j"); done
    for k in "${cands[@]}"; do
      F=$k-$slug.patch
      [[ "$F" > "$lo" ]] || continue
      [ -z "$hi" ] || [[ "$F" < "$hi" ]] || continue
      [ -z "${taken[$k]:-}" ] || continue
      clash=0
      for f in "${FILES[@]}"; do [ "${FKEY[$f]}" != "$k" ] || [ "$f" = "${MATCH[i]}" ] || clash=1; done
      [ $clash -eq 0 ] || continue
      NAME[i]=$F; taken[$k]=1; break
    done
    [ -n "${NAME[i]}" ] || die "no free name between ${lo:-the start} and ${hi:-the end} for '$(subj "${CM[i]}")'"
  done
}

# regen <base> <stack> <dir>: format-patch every stack commit; RSUB[subject] = file
regen() {
  local f
  rm -rf "$3"; mkdir -p "$3"
  git -c diff.algorithm=myers -c diff.indentHeuristic=true -c diff.noprefix=false -c diff.mnemonicPrefix=false \
      -c format.subjectPrefix=PATCH -c diff.relative=false \
    format-patch -q -U3 --inter-hunk-context=0 --zero-commit -N --no-signature --no-stat --full-index --no-renames \
    -o "$3" "$1..$2" >/dev/null
  declare -gA RSUB=()
  for f in "$3"/*.patch; do RSUB[$(mi_subject "$f")]=${f##*/}; done
}

# place <dir> <regen dir> <git|plain>: write NAME/MATCH into dir; an unclaimed active
# file whose change is already in the base becomes .merged. Log: $T/changes
place() {
  local dir=$1 rg=$2 mode=$3 i o s cj f
  : > "$T/changes"
  mv_() { if [ "$mode" = git ]; then git -C "$dir" mv -f -- "$1" "$2"; else mv -f "$dir/$1" "$dir/$2"; fi; }
  for ((i = 0; i < ${#CM[@]}; i++)); do
    s=$(subj "${CM[i]}")
    f=${RSUB[$s]:-}; [ -n "$f" ] || die "no regenerated patch for '$s'"
    if [ -n "${MATCH[i]}" ] && [ "${MATCH[i]}" != "${NAME[i]}" ]; then
      mv_ "${MATCH[i]}" "${NAME[i]}"
      case "${MATCH[i]}" in
        *.patch) echo "renamed ${MATCH[i]} -> ${NAME[i]}" ;;
        *) echo "re-enabled ${MATCH[i]} -> ${NAME[i]}" ;;
      esac >> "$T/changes"
    elif [ -z "${MATCH[i]}" ]; then
      echo "added ${NAME[i]}" >> "$T/changes"
    fi
    cp "$rg/$f" "$dir/${NAME[i]}"
  done
  for o in "${LEFT[@]}"; do
    cj=$NB
    for ((i = 0; i < ${#CM[@]}; i++)); do [[ "${NAME[i]}" > "$o" ]] || cj=${CM[i]}; done
    if reverse_applies "$NB" "$dir/$o" && reverse_applies "$cj" "$dir/$o"; then
      mv_ "$o" "$o.merged"; echo "merged $o -> $o.merged" >> "$T/changes"
    elif reverse_applies "$cj" "$dir/$o"; then
      die "active patch $o has no commit and its change is not upstream: it was squashed into another patch or dropped - disable it on yacer instead"
    else
      die "active patch $o has no commit: disable it on yacer instead of dropping its commit"
    fi
  done
  [ "$mode" != git ] || git -C "$dir" add -A .
}

# replay <base> <stack> <dir> <return to>: the files, applied to the base the way
# the build applies them, give exactly the stack's tree
replay() {
  local rc
  set +e
  ( set -e; replay_body "$@" )
  rc=$?
  set -e
  git checkout -q -f "$4"; git clean -fdxq
  [ $rc -eq 0 ] || exit $rc
}
replay_body() {
  local f
  git checkout -q -f --detach "$1"; git clean -fdxq
  for f in "$3"/*.patch; do
    apply_unpack "$f" || die "replay: ${f##*/} does not apply to ${1:0:12}"
  done
  [ -z "$(git ls-files -o -- '*.rej' '*.orig')" ] || die "replay: patches applied with fuzz or rejects"
  git add -A -f
  git diff --cached --quiet "$2" || die "replay: the regenerated patches do not rebuild ${2:0:12}"
}

identity() {
  local c msg
  for c in "$@"; do
    [ "$(git log -1 --format='%ae %ce' "$c")" = "$MAIL $MAIL" ] \
      || die "${c:0:12} ($(subj "$c")) is not authored and committed as $ME <$MAIL>"
    msg=$(git log -1 --format=%B "$c")
    grep -qxF "$SOB" <<<"$msg" || die "${c:0:12} ($(subj "$c")) has no '$SOB'"
    ! grep -qiE "$ATTR" <<<"$msg" || die "${c:0:12} ($(subj "$c")) carries attribution text"
  done
}
# patch_hygiene <dir>: safe paths; no attribution in headers, messages or added lines
patch_hygiene() {
  local f
  for f in "$1"/*.patch; do
    screen "$f" "${f##*/}"
    awk '/^---$|^diff --git /{body=1} !body || (/^\+/ && !/^\+\+\+ /)' "$f" > "$T/hy"
    ! grep -qiE "$ATTR" "$T/hy" || die "${f##*/} carries attribution text"
  done
}

# write_source <dir> <file> <branch> <kodi>
write_source() {
  local f
  {
    echo "branch $3"
    echo "kodi $4"
    for f in "$1"/*.patch; do
      echo "patch $(git hash-object --no-filters "$f") ${f##*/} $(mi_subject "$f")"
    done
  } > "$2"
}

# partial <old base> <old stack> <new base> <new stack>: subjects whose change,
# not only its context, differs after the rebase
partial() {
  local c s o oc os
  git log --format='%H %s' "$1..$2" > "$T/old"
  git log --reverse --format='%H %s' "$3..$4" > "$T/new"
  while read -r c s; do
    o=""
    while read -r oc os; do [ "$os" != "$s" ] || { o=$oc; break; }; done < "$T/old"
    [ -n "$o" ] || continue
    [ "$(pid "$o")" != "$(pid "$c")" ] || continue
    [ "$(sig "$o")" = "$(sig "$c")" ] || echo "$s"
  done < "$T/new"
}
pid() { git show "$1" > "$T/pid.show"; git patch-id --stable < "$T/pid.show" | cut -d' ' -f1; }
sig() {
  git show --format= -U0 "$1" > "$T/sig.show"
  { grep -E '^[-+]' "$T/sig.show" || true; } | { grep -vE '^(\+\+\+|---) ' || true; } | sort | sha1sum
}

# --------------------------------------------------------------- CI sync
WORK=${RUNNER_TEMP:-}
emit() { printf '%s\n' "$@" >> "$PLAN"; }

# attempt <CoreELEC target> <label>: rebase yacer-kodi onto the target and commit
# the regenerated yacer files in a worktree of the yacer checkout.
#   out: $T/attempt.<label> (tip=, yacer=); on failure exit 1, with $T/needs.<label>
#        non-empty when a person has to decide
attempt() {
  local tgt=$1 R OB R2 T2 cyw ycommit dropped s
  local -a what=()
  NEEDS=$T/needs.$2; : > "$NEEDS"
  cd "$XBW"
  TGT=$tgt; CEREPO=$YR; MIR=$M
  git rebase --abort >/dev/null 2>&1 || true
  git checkout -q -f --detach "$TIP"; git clean -fdxq
  R=$(git rev-parse "$TIP^2"); OB=$(obof "$R"); OLDPIN=$(git rev-parse "$OB^")
  git branch -f yacer-kodi-work "$R"
  TODO=$T/todo.$2; SKIPS=$T/skips.$2; : > "$SKIPS"
  base "$YR" "$tgt" "$OB"
  mktodo "$OB" "$R"
  rebase_onto "$NB" "$OB"
  loop
  post_asserts "$NB"
  R2=$(git rev-parse HEAD)
  git log --reverse --format=%H "$NB..$R2" > "$T/commits"
  cyw=$WORK/yacer-$2
  git -C "$YR" worktree remove --force "$cyw" >/dev/null 2>&1 || true
  rm -rf "$cyw"; git -C "$YR" worktree prune
  git -C "$YR" worktree add -q --detach "$cyw" HEAD
  names "$cyw/$Y"
  if [ "$NB" != "$OB" ]; then
    partial "$OB" "$R" "$NB" "$R2" > "$T/partial"
    if [ -s "$T/partial" ]; then
      echo "$PARTLY_HINT" >> "$T/partial"
      report_block "### partly merged upstream, needs review:" "$T/partial"
      needs_human "a patch changed beyond its context in the rebase: resolve it with yacer-kodi.sh start --upstream"
    fi
  fi
  regen "$NB" "$R2" "$T/regen"
  place "$cyw/$Y" "$T/regen" git
  replay "$NB" "$R2" "$cyw/$Y" "$R2"
  if [ "$R2" = "$R" ]; then
    T2=$TIP
  else
    [ "$NB" = "$OB" ] || what+=("rebase onto CoreELEC/xbmc ${PIN:0:12}")
    git log --reverse --format=%s "$OB..$R" > "$T/oldsubjects"
    dropped=""
    while IFS= read -r s; do
      grep -Fxq -- "$s" "$T/drops" || continue
      dropped+="${dropped:+ }$(name_for_subject "$s")"
    done < "$T/oldsubjects"
    [ -z "$dropped" ] || what+=("drop $dropped")
    [ ${#what[@]} -gt 0 ] || what+=("update the stack")
    T2=$(git commit-tree "$R2^{tree}" -p "$TIP" -p "$R2" -m "kodi: $(printf '%s; ' "${what[@]}" | sed 's/; $//')" -m "$SOB")
    identity "$T2"
  fi
  [ "$NB" = "$R2" ] || identity $(git rev-list "$NB..$R2")
  patch_hygiene "$cyw/$Y"
  write_source "$cyw/$Y" "$cyw/kodi.source" "$T2" "$(kodiid "$YR" "$tgt")"
  git -C "$cyw" add -A -- "$Y" kodi.source
  ycommit=""
  if ! git -C "$cyw" diff --cached --quiet; then
    { echo "kodi: regenerate patches from yacer-kodi"; echo; echo "yacer-kodi ${T2:0:12}, CoreELEC ${tgt:0:12}"
      cat "$T/changes"; } > "$T/ymsg"
    git -C "$cyw" commit -q -s --cleanup=whitespace -F "$T/ymsg"
    ycommit=$(git -C "$cyw" rev-parse HEAD)
    (cd "$cyw" && identity "$ycommit")
  fi
  if [ "$NB" != "$OB" ]; then
    git diff -z --name-only "$NB" "$R2" > "$T/touched"
    mapfile -d '' -t files < "$T/touched"
    upstream_commits "$OLDPIN" "$PIN" ${files[@]+"${files[@]}"}
    [ ! -s "$T/uplog" ] || report_block "new upstream kodi commits on files our patches touch:" "$T/uplog"
  fi
  [ ! -s "$T/changes" ] || report_block "yacer files:" "$T/changes"
  printf 'tip=%s\nyacer=%s\n' "$T2" "$ycommit" > "$T/attempt.$2"
}

emit_attempt() {
  local tip yc
  tip=$(sed -n 's/^tip=//p' "$T/attempt.$1"); yc=$(sed -n 's/^yacer=//p' "$T/attempt.$1")
  [ "$tip" = "$TIP" ] || emit "xbmc=$tip"
  [ -z "$yc" ] || emit "yacer=$yc"
}

plan_body() {
  local KU rc res c stackkodi
  cd "$YR"
  refuse_home "$YR" "$WORK"
  need_remote "$YR" origin "$URL_CE"; need_config "$YR"; config_allowlist "$YR"
  git fetch -q --depth=1 origin +refs/heads/coreelec-22:refs/yacer/mirror
  git fetch -q --depth=1 "$URL_UP" +refs/heads/coreelec-22:refs/yacer/upstream
  M=$(git rev-parse refs/yacer/mirror); U=$(git rev-parse refs/yacer/upstream)
  load_source "$YR/kodi.source"
  [ "$(kodiid "$YR" "$M")" = "$K" ] || report "::warning::mirror ${M:0:12} does not match kodi.source"
  listing "$YR" HEAD
  intent
  KU=$(kodiid "$YR" "$U")
  git ls-remote "$URL_XBMC" refs/heads/yacer-kodi > "$T/lsremote"
  TIP=$(cut -f1 "$T/lsremote")
  sha_ok "$TIP" || die "no yacer-kodi on $URL_XBMC"
  if [ "$KU" = "$K" ] && [ "$TIP" = "$S" ] && [ ! -s "$T/drops" ]; then
    [ "$U" = "$M" ] || { emit "mirror=$U" "lease=$M"; report "mirror ${M:0:12} -> ${U:0:12} (kodi unchanged)"; }
    emit result=ok
    return
  fi
  XBW=$WORK/xbmc
  rm -rf "$XBW"
  git clone -q --single-branch -b yacer-kodi "$URL_XBMC" "$XBW"
  git -C "$XBW" remote add coreelec "$URL_CXBMC"
  config_allowlist "$XBW"
  TIP=$(git -C "$XBW" rev-parse HEAD)
  emit "xb=$XBW"
  git -C "$XBW" merge-base --is-ancestor "$S" "$TIP" \
    || die "kodi.source names ${S:0:12}, which is not on yacer-kodi (land not pushed?)"
  git -C "$XBW" rev-list --first-parent "$S..$TIP" > "$T/tips"
  echo "$TIP" >> "$T/tips"
  while read -r c; do
    [ "$(git -C "$XBW" rev-list --parents -n1 "$c" | wc -w)" -eq 3 ] \
      && [ "$(git -C "$XBW" rev-parse "$c^{tree}")" = "$(git -C "$XBW" rev-parse "$c^2^{tree}")" ] \
      || die "yacer-kodi ${c:0:12} is not a two-parent commit with its stack's tree"
  done < "$T/tips"
  set +e; ( set -e; attempt "$U" upstream ); rc=$?; set -e
  if [ $rc -eq 0 ]; then
    emit_attempt upstream
    [ "$U" = "$M" ] || emit "mirror=$U" "lease=$M"
    emit result=ok
    return
  fi
  res=fail; [ ! -s "$T/needs.upstream" ] || res=conflict
  [ "$res" = conflict ] || report "::error::the sync failed (exit $rc) before any decision a person has to make - see the log above"
  if [ "$KU" != "$K" ] && [ "$res" = conflict ]; then
    stackkodi=$(cd "$XBW" && kodiline "$(obof "$TIP^2")")
    if [ "$stackkodi" = "$(kodiid "$YR" "$M")" ]; then
      report "### upstream does not fit; landing pending changes on the held mirror ${M:0:12}"
      set +e; ( set -e; attempt "$M" mirror ); rc=$?; set -e
      if [ $rc -ne 0 ]; then
        report "the held-mirror attempt failed too (exit $rc); nothing lands"
      else
        emit_attempt mirror
        grep -qE '^(xbmc|yacer)=' "$PLAN" || report "nothing was pending; the mirror stays at ${M:0:12}"
      fi
    else
      report "### held-mirror fallback skipped: yacer-kodi is already on another CoreELEC kodi than the mirror (a landed rebase waits for upstream)"
    fi
  fi
  emit "result=$res"
}

cmd_plan() {
  [ -n "$CIMODE" ] && [ -n "$WORK" ] || die "plan runs in CI (RUNNER_TEMP)"
  exec 3>&1 1>&2
  PLAN=$T/plan; : > "$PLAN"
  local rc
  set +e
  ( set -e; plan_body )
  rc=$?
  set -e
  if ! grep -q '^result=' "$PLAN"; then
    [ $rc -ne 0 ] || die "plan ended without a result"
    : > "$PLAN"; emit result=fail
  fi
  [ -z "${GITHUB_OUTPUT:-}" ] || grep '^result=' "$PLAN" >> "$GITHUB_OUTPUT"
  report "sync $(grep '^result=' "$PLAN")"
  cat "$PLAN" >&3
}

# config_allowlist <repo>: nothing in the local config beyond what a clone writes
config_allowlist() {
  local bad
  bad=$(git -C "$1" config --local --list \
    | { grep -vE '^(core\.(repositoryformatversion|filemode|bare|logallrefupdates|ignorecase|precomposeunicode)|extensions\.[a-z]+|remote\.(origin|coreelec)\.(url|fetch)|branch\.(yacer|yacer-kodi)\.(remote|merge)|gc\.auto)=' || true; })
  [ -z "$bad" ] || die "$1 has unexpected local config: $bad"
}

validate_plan() {
  local line k v
  while IFS= read -r line; do
    k=${line%%=*}; v=${line#*=}
    case "$k" in
      xb) [ "$v" = "$WORK/xbmc" ] || die "plan: bad xb $v" ;;
      xbmc|yacer|mirror|lease) sha_ok "$v" || die "plan: bad $k $v" ;;
      result) case "$v" in ok|conflict|fail) ;; *) die "plan: bad result $v" ;; esac ;;
      *) die "plan: unexpected line $line" ;;
    esac
  done < "$1"
}

cmd_push() {
  { set +x; } 2>/dev/null
  local plan=${1:?usage: push <plan>} xb xbmc yacer mirror lease kh changed=false
  local -a refs=()
  [ -n "$CIMODE" ] && [ -n "$WORK" ] || die "push runs in CI (RUNNER_TEMP)"
  refuse_home "$YR" "$WORK"
  validate_plan "$plan"
  get() { sed -n "s/^$1=//p" "$plan" | sed -n '$p'; }
  xb=$(get xb); xbmc=$(get xbmc); yacer=$(get yacer); mirror=$(get mirror); lease=$(get lease)
  pushed() { changed=true; [ -z "${GITHUB_OUTPUT:-}" ] || echo "changed=true" >> "$GITHUB_OUTPUT"; report "pushed $*"; }
  need_remote "$YR" origin "$URL_CE"; config_allowlist "$YR"
  if [ -n "$xbmc" ]; then
    [ -n "$xb" ] || die "plan: xbmc without xb"
    config_allowlist "$xb"
    if [ -n "${XBMC_DEPLOY_KEY:-}" ]; then
      KEYF=$WORK/xbmc-deploy-key; kh=$WORK/known_hosts
      (umask 077; printf '%s\n' "$XBMC_DEPLOY_KEY" > "$KEYF")
      echo "$KNOWN_HOST" > "$kh"
      GIT_SSH_COMMAND="ssh -F /dev/null -o BatchMode=yes -o IdentitiesOnly=yes -o StrictHostKeyChecking=yes"
      GIT_SSH_COMMAND+=" -i $(printf %q "$KEYF") -o UserKnownHostsFile=$(printf %q "$kh")"
      export GIT_SSH_COMMAND
    elif [ -z "${YACER_PUSH_XBMC:-}" ]; then
      die "XBMC_DEPLOY_KEY is not set"
    fi
    git -C "$xb" push -q "$PUSH_XBMC" "$xbmc:refs/heads/yacer-kodi"
    pushed "yacer-kodi ${xbmc:0:12}"
    [ "$(git -C "$xb" ls-remote "$PUSH_XBMC" refs/heads/yacer-kodi | cut -f1)" = "$xbmc" ] \
      || die "yacer-kodi is not at ${xbmc:0:12} after the push"
  fi
  cd "$YR"
  if [ -n "${GH_TOKEN:-}" ]; then
    force_config http.https://github.com/.extraheader \
      "AUTHORIZATION: basic $(printf 'x-access-token:%s' "$GH_TOKEN" | base64 -w0)"
  fi
  if [ -n "$mirror" ]; then
    # receive-pack drops --atomic for a ref that needs a shallow update, so the
    # upstream objects go up first on a branch nothing builds
    git push -q -f origin "$mirror:$STAGE"
    [ -z "$yacer" ] || refs+=("$yacer:refs/heads/yacer")
    git push -q --atomic origin "${refs[@]}" "--force-with-lease=refs/heads/coreelec-22:$lease" \
      "$mirror:refs/heads/coreelec-22" ":$STAGE"
    pushed "${yacer:+yacer ${yacer:0:12} and }coreelec-22 ${mirror:0:12}"
  elif [ -n "$yacer" ]; then
    git push -q origin "$yacer:refs/heads/yacer"
    pushed "yacer ${yacer:0:12}"
  fi
  if [ -n "$mirror$yacer" ]; then
    git ls-remote origin refs/heads/yacer refs/heads/coreelec-22 > "$T/after"
    [ -z "$yacer" ] || grep -qx "$yacer"$'\trefs/heads/yacer' "$T/after" || die "yacer is not at ${yacer:0:12} after the push"
    [ -z "$mirror" ] || grep -qx "$mirror"$'\trefs/heads/coreelec-22' "$T/after" || die "coreelec-22 is not at ${mirror:0:12} after the push"
  fi
  [ "$changed" = true ] || [ -z "${GITHUB_OUTPUT:-}" ] || echo "changed=false" >> "$GITHUB_OUTPUT"
  echo "changed=$changed"
}

# ------------------------------------------------------------------ guard
cmd_guard() {
  if [ "${1:-}" = --local ]; then guard_local "${2:-}"; return; fi
  local mirror result=ok blob name subject cur tip
  local -A dblob=() pblob=()
  out() { [ -z "${GITHUB_OUTPUT:-}" ] || echo "$1" >> "$GITHUB_OUTPUT"; echo "$1"; }
  gfail() {
    tip=$(git -C "$YR" ls-remote origin refs/heads/yacer 2>/dev/null | cut -f1 || true)
    if [ -n "$tip" ] && [ "$tip" != "$(git -C "$YR" rev-parse HEAD)" ]; then
      report "yacer has moved on since this commit ($*); its own build decides"
      out result=skip; exit 0
    fi
    out result=fail; report "::error::$*"; exit 1
  }
  mirror=$(git rev-parse HEAD)
  out "mirror=$mirror"
  [ -s "$YR/kodi.source" ] || gfail "no kodi.source on yacer"
  K=$(sed -n 's/^kodi //p' "$YR/kodi.source")
  sed -n 's/^patch //p' "$YR/kodi.source" > "$T/P"
  [ -n "$K" ] && grep -q '^branch ' "$YR/kodi.source" || gfail "kodi.source is incomplete"
  [ "$(kodiid . "$mirror")" = "$K" ] || gfail "the kodi patches on yacer were not made for mirror ${mirror:0:12} - wait for the sync"
  listing "$YR" HEAD
  while read -r name cur; do dblob[$name]=$cur; done < "$T/D"
  while read -r blob name subject; do pblob[$name]=$blob; done < "$T/P"
  for name in "${!dblob[@]}"; do
    case "$name" in *.patch) ;; *) continue ;; esac
    [ "${pblob[$name]:-}" = "${dblob[$name]}" ] || gfail "$name is not what the sync wrote - kodi patches change through yacer-kodi"
  done
  for name in "${!pblob[@]}"; do [ -n "${dblob[$name]:-}" ] || result=skip; done
  [ "$result" = ok ] || report "kodi patch removals are pending: the sync drops them and starts a build"
  out "result=$result"
}

guard_local() {
  local src=$1 bad
  [ -n "$src" ] || src=${YACER_KODI_PATCHES:+$YACER_KODI_PATCHES/kodi.source}
  [ -n "$src" ] || src=$YR/kodi.source
  load_source "$src"
  [ "$(kodiid . HEAD)" = "$K" ] || die "the kodi patches were made for another CoreELEC kodi (kodi.source: $K); check out the matching mirror"
  git status --porcelain --untracked-files=all -- "$KPKG/package.mk" "$K1" "$K2" "$K3" > "$T/st"
  bad=$({ grep -v '^?? ' "$T/st" || true; grep '^?? .*\.patch$' "$T/st" || true; })
  [ -z "$bad" ] || die "CoreELEC kodi files are modified locally:"$'\n'"$bad"
}

# ------------------------------------------------------------------ local
wt_paths() { TODO=$(gitpath yacer-todo); SKIPS=$(gitpath yacer-skips); [ -f "$SKIPS" ] || : > "$SKIPS"; }

# local_intent: kodi.source, listing and drops from the local yacer HEAD (D1)
local_intent() {
  git -C "$CY" show HEAD:kodi.source > "$T/src.local" 2>/dev/null || die "no kodi.source in $CY HEAD"
  load_source "$T/src.local"
  listing "$CY" HEAD
  intent
}
same_source_as_origin() {
  git -C "$CE" show refs/remotes/origin/yacer:kodi.source > "$T/src.origin" 2>/dev/null || die "no kodi.source on origin/yacer"
  cmp -s "$T/src.local" "$T/src.origin" || die "update local yacer: its kodi.source differs from origin/yacer"
}

conflict_context() {
  local R OB NB
  CEREPO=$CE
  MIR=$(git -C "$CE" rev-parse refs/remotes/origin/coreelec-22)
  R=$(git -C "$XB" rev-parse refs/yacer/start^2); OB=$(obof "$R"); OLDPIN=$(git rev-parse "$OB^")
  NB=$(obof HEAD); PIN=$(git rev-parse "$NB^")
  TGT=$(git log -1 --format=%B "$NB" | sed -n 's/^CoreELEC //p')
  if git -C "$CY" show HEAD:kodi.source > "$T/src.cc" 2>/dev/null; then sed -n 's/^patch //p' "$T/src.cc" > "$T/P"; else : > "$T/P"; fi
}

after_loop() {
  local R OB NB
  R=$(git -C "$XB" rev-parse refs/yacer/start^2); OB=$(obof "$R"); NB=$(obof HEAD)
  local_intent
  if [ -f "$TODO" ]; then post_asserts "$NB"; rm -f "$TODO"; fi
  check_subjects "$NB" HEAD
  say "old base ${OB:0:12}, new base ${NB:0:12}"
  if [ "$NB" != "$OB" ]; then
    partial "$OB" "$R" "$NB" HEAD > "$T/partial"
    [ ! -s "$T/partial" ] || say "changed beyond context by the rebase - review:"$'\n'"$(sed 's/^/    /' "$T/partial")"$'\n'"  $PARTLY_HINT"
  fi
  git --no-pager range-diff "$OB..$R" "$NB..HEAD" >&2 || true
  say "next: review, then yacer-kodi.sh land -m <message>"
}

cleanup_work() {
  cd "$XB"
  git worktree remove --force "$WT" >/dev/null 2>&1 || rm -rf "$WT"
  git worktree prune
  rm -rf "$OUT"
  git update-ref -d refs/yacer/start 2>/dev/null || true
}

fresh_body() {
  local R OB
  cd "$WT"; wt_paths; : > "$SKIPS"
  R=$(git rev-parse HEAD); OB=$(obof "$R"); OLDPIN=$(git rev-parse "$OB^")
  CEREPO=$CE; MIR=$(git -C "$CE" rev-parse refs/remotes/origin/coreelec-22)
  base "$CE" "$TGT" "$OB"
  mktodo "$OB" "$R"
  rebase_onto "$NB" "$OB"
  loop
  after_loop
}

cmd_start() {
  local up="" tipr rc
  [ "${1:-}" != --upstream ] || up=1
  local_env; fetch_all; check_local_branch
  if [ -d "$WT" ]; then
    cd "$WT"; wt_paths
    if busy; then conflict_context; loop; after_loop; return; fi
    [ "$(git -C "$XB" rev-parse refs/remotes/origin/yacer-kodi)" = "$(git -C "$XB" rev-parse -q --verify refs/yacer/start || true)" ] \
      || die "tip moved: yacer-kodi.sh abandon && yacer-kodi.sh start${up:+ --upstream} (rerere replays your resolutions)"
    if [ -f "$TODO" ]; then after_loop; return; fi
    say "ready to land: yacer-kodi.sh land -m <message>"
    return
  fi
  tipr=$(git -C "$XB" rev-parse refs/remotes/origin/yacer-kodi)
  local_intent
  same_source_as_origin
  if [ "$S" != "$tipr" ]; then
    git -C "$XB" merge-base --is-ancestor "$S" "$tipr" 2>/dev/null \
      || die "kodi.source names ${S:0:12}, which is not on origin/yacer-kodi"
    say "warning: yacer-kodi has a landed change the sync has not written yet; starting from it"
  fi
  if [ -n "$up" ]; then TGT=$(git -C "$CE" rev-parse refs/remotes/upstream/coreelec-22)
  else TGT=$(git -C "$CE" rev-parse refs/remotes/origin/coreelec-22); fi
  git -C "$XB" worktree add -q --no-checkout -B yacer-kodi-work "$WT" "$tipr^2"
  git -C "$XB" update-ref refs/yacer/start "$tipr"
  set +e; ( set -e; fresh_body ); rc=$?; set -e
  if [ $rc -ne 0 ]; then
    (cd "$WT" && busy) || cleanup_work
    exit $rc
  fi
}

cmd_enable() {
  local f=${1:?usage: enable <file>} before rc=0
  f=${f##*/}
  local_env
  case "$f" in
    *.patch) die "$f is already active" ;;
    *.patch.disabled|*.patch.merged) ;;
    *) die "$f is not a .patch.disabled or .patch.merged file" ;;
  esac
  git -C "$CY" cat-file -e "HEAD:$Y/$f" 2>/dev/null || die "no $Y/$f in $CY HEAD"
  git -C "$CY" show "HEAD:$Y/$f" > "$T/enable.patch"
  [ -n "$(mi_subject "$T/enable.patch")" ] || die "$f is not a mail patch, apply it by hand (patch -p1, commit -s)"
  if [ -d "$WT" ]; then (cd "$WT" && ! busy) || die "a rebase is in progress: finish it with start first"
  else (cmd_start); fi
  cd "$WT"; wt_paths
  before=$(git rev-parse HEAD)
  git am -3 --whitespace=nowarn "$T/enable.patch" >&2 || rc=$?
  if [ $rc -eq 0 ]; then
    [ "$(git rev-parse HEAD)" != "$before" ] || { say "$f: no changes - already upstream, leave it as it is"; return; }
    say "enabled $(subj HEAD) on top of the stack; reorder with git rebase -i if needed, then land"
    return
  fi
  conflict_context
  loop
}

cmd_land() {
  local msg R R2 OB NB T2 T3 start old
  [ "${1:-}" = -m ] && [ -n "${2:-}" ] || die "usage: land -m <message>"
  msg=$2
  local_env
  git -C "$XB" fetch -q origin +refs/heads/yacer-kodi:refs/remotes/origin/yacer-kodi
  git -C "$CE" fetch -q origin +refs/heads/yacer:refs/remotes/origin/yacer
  check_local_branch
  [ -d "$WT" ] || die "nothing to land: run start first"
  cd "$WT"; wt_paths
  ! busy || die "a rebase or am is in progress: finish it with start"
  start=$(git -C "$XB" rev-parse -q --verify refs/yacer/start || true)
  [ "$(git -C "$XB" rev-parse refs/remotes/origin/yacer-kodi)" = "$start" ] \
    || die "tip moved: yacer-kodi.sh abandon && yacer-kodi.sh start (rerere replays your resolutions)"
  [ "$(git symbolic-ref -q HEAD || true)" = refs/heads/yacer-kodi-work ] || die "check out yacer-kodi-work first"
  [ -z "$(git status --porcelain)" ] || die "uncommitted changes in $WT"
  [ ! -f "$TODO" ] || die "start did not finish: run start again"
  ! git -C "$XB" worktree list --porcelain | grep -qx 'branch refs/heads/yacer-kodi' \
    || die "yacer-kodi is checked out in a worktree of $XB; switch that worktree away first"
  local_intent
  same_source_as_origin
  R=$(git rev-parse "$start^2"); R2=$(git rev-parse HEAD)
  [ "$R2" != "$R" ] || die "nothing to land: the stack is unchanged"
  OB=$(obof "$R"); NB=$(obof "$R2")
  [ -z "$(git rev-list --merges "$NB..$R2")" ] || die "the stack has merge commits"
  if [ "$NB" != "$OB" ]; then
    T3=$(partly_upstream "$OB" "$R" "$NB" "$R2")
    if [ "$T3" != "$R2" ]; then git reset -q --hard "$T3"; R2=$T3; fi
  fi
  check_subjects "$NB" "$R2"
  git log --reverse --format=%H "$NB..$R2" > "$T/commits"
  mkdir -p "$T/cy"; git -C "$CY" archive HEAD "$Y" | tar -x -C "$T/cy"
  names "$T/cy/$Y"
  regen "$NB" "$R2" "$T/regen"
  place "$T/cy/$Y" "$T/regen" plain
  say "names the sync will write:"
  say "$(sed 's/^/    /' "$T/changes")"
  if [ "$NB" != "$OB" ]; then
    partial "$OB" "$R" "$NB" "$R2" > "$T/partial"
    [ ! -s "$T/partial" ] || say "changed beyond context - make sure this is intended:"$'\n'"$(sed 's/^/    /' "$T/partial")"$'\n'"  $PARTLY_HINT"
  fi
  replay "$NB" "$R2" "$T/cy/$Y" yacer-kodi-work
  [ "$NB" = "$R2" ] || identity $(git rev-list "$NB..$R2")
  patch_hygiene "$T/cy/$Y"
  ! grep -qiE "$ATTR" <<<"$msg" || die "the message carries attribution text"
  T2=$(git commit-tree "$R2^{tree}" -p "$start" -p "$R2" -m "$msg" -m "$SOB")
  identity "$T2"
  old=$(git -C "$XB" rev-parse -q --verify refs/heads/yacer-kodi || true)
  git -C "$XB" update-ref refs/heads/yacer-kodi "$T2" "$old"
  cleanup_work
  say "landed yacer-kodi ${T2:0:12}. Confirm each push:"
  say "  git -C $XB push origin yacer-kodi"
  if ! git -C "$CY" diff --quiet refs/remotes/origin/yacer HEAD -- "$Y"; then
    say "  git -C $CY push origin yacer        (starts the sync)"
  else
    say "  gh workflow run yacer-sync.yml -R allolive/CoreELEC"
  fi
}

cmd_patches() {
  local R2 NB tip
  local_env
  if [ -d "$WT" ]; then
    cd "$WT"
    ! busy || die "a rebase or am is in progress: finish it with start"
    R2=$(git rev-parse HEAD); tip=$R2
  else
    cd "$XB"
    tip=$(git rev-parse -q --verify refs/heads/yacer-kodi || git rev-parse refs/remotes/origin/yacer-kodi)
    R2=$(git rev-parse "$tip^2")
  fi
  NB=$(obof "$R2")
  git log --reverse --format=%H "$NB..$R2" > "$T/commits"
  mkdir -p "$T/cy"; git -C "$CY" archive HEAD "$Y" | tar -x -C "$T/cy"
  names "$T/cy/$Y"
  regen "$NB" "$R2" "$T/regen"
  place "$T/cy/$Y" "$T/regen" plain
  rm -rf "$OUT"; mkdir -p "$OUT"
  for f in "$T/cy/$Y"/*.patch; do cp "$f" "$OUT"/; done
  write_source "$OUT" "$OUT/kodi.source" "$tip" "$(kodiline "$NB")"
  [ ! -s "$T/changes" ] || say "$(cat "$T/changes")"
  say "wrote $(find "$OUT" -name '*.patch' | wc -l) patch(es) and kodi.source to $OUT"
}

cmd_abandon() {
  local ref ts
  local_env
  if [ ! -d "$WT" ]; then cleanup_work; say "nothing to abandon"; return; fi
  cd "$WT"
  ref=$(git rev-parse -q --verify HEAD || true)
  [ ! -d "$(gitpath rebase-merge)" ] || git rebase --abort >/dev/null 2>&1 || true
  [ ! -d "$(gitpath rebase-apply)" ] || git am --abort >/dev/null 2>&1 || true
  ts=$(date +%Y%m%d%H%M%S)
  while git -C "$XB" rev-parse -q --verify "refs/yacer/abandoned/$ts" >/dev/null; do ts=$ts-1; done
  [ -z "$ref" ] || git -C "$XB" update-ref "refs/yacer/abandoned/$ts" "$ref"
  cleanup_work
  say "abandoned${ref:+; the work is kept as refs/yacer/abandoned/$ts}"
}

cmd_import() {
  local cec yc f name email date subject R0 T0 rc
  local_env
  ! git -C "$XB" rev-parse -q --verify refs/heads/yacer-kodi >/dev/null || die "yacer-kodi already exists in $XB"
  [ ! -d "$WT" ] || die "local work exists in $WT"
  cec=$(git -C "$CE" rev-parse -q --verify "${1:-refs/remotes/origin/coreelec-22}^{commit}") || die "no CoreELEC commit ${1:-}"
  yc=$(git -C "$CY" rev-parse -q --verify "${2:-HEAD}^{commit}") || die "no yacer commit ${2:-}"
  mkdir -p "$T/src"; git -C "$CY" archive "$yc" "$Y" | tar -x -C "$T/src"
  git -C "$XB" worktree add -q --no-checkout --detach "$WT" "$(git -C "$XB" rev-parse -q --verify HEAD || git -C "$XB" rev-list -n1 --all)"
  set +e
  (
    set -e
    cd "$WT"
    base "$CE" "$cec"
    for f in "$T/src/$Y"/*.patch; do
      apply_unpack "$f" || die "${f##*/} does not apply"
      [ -z "$(git ls-files -o -- '*.rej')" ] || die "${f##*/} left .rej files"
      git ls-files -o -z -- '*.orig' | xargs -0r rm -f
      git add -A -f
      git mailinfo -b "$T/msg" "$T/diff" < "$f" > "$T/info"
      name=$(sed -n 's/^Author: //p' "$T/info"); email=$(sed -n 's/^Email: //p' "$T/info")
      date=$(sed -n 's/^Date: //p' "$T/info"); subject=$(sed -n 's/^Subject: //p' "$T/info")
      [ -n "$subject" ] || die "${f##*/} has no Subject"
      [ "${email:-$MAIL}" = "$MAIL" ] || die "${f##*/} is authored by $email: the branch carries only $ME <$MAIL> - fix the patch first"
      { echo "$subject"; echo; cat "$T/msg"; } > "$T/cmsg"
      ! git diff --cached --quiet || die "${f##*/} changes nothing"
      if [ -n "$date" ]; then export GIT_AUTHOR_DATE=$date; else unset GIT_AUTHOR_DATE; fi
      GIT_AUTHOR_NAME=${name:-$ME} git commit -q -s --cleanup=whitespace -F "$T/cmsg"
    done
    unset GIT_AUTHOR_DATE
    R0=$(git rev-parse HEAD)
    check_subjects "$NB" "$R0"
    [ "$NB" = "$R0" ] || identity $(git rev-list "$NB..$R0")
    git log --reverse --format=%H "$NB..$R0" > "$T/commits"
    cp -a "$T/src/$Y" "$T/names"
    names "$T/names"
    regen "$NB" "$R0" "$T/regen"
    place "$T/names" "$T/regen" plain
    replay "$NB" "$R0" "$T/names" "$R0"
    patch_hygiene "$T/names"
    T0=$(git commit-tree "$R0^{tree}" -p "$PIN" -p "$R0" -m "kodi: import yacer patches onto CoreELEC/xbmc ${PIN:0:12}" -m "$SOB")
    identity "$T0"
    git -C "$XB" update-ref refs/heads/yacer-kodi "$T0" ""
    say "yacer-kodi ${T0:0:12}: $(wc -l < "$T/commits") patch commit(s) on base ${NB:0:12} (kodi ${PIN:0:12})"
    [ ! -s "$T/changes" ] || say "$(cat "$T/changes")"
    local fmt=0
    for f in "$T/names"/*.patch; do
      cmp -s "$f" "$T/src/$Y/${f##*/}" && continue
      if [ "$(git patch-id --stable < "$f" | cut -d' ' -f1)" = "$(git patch-id --stable < "$T/src/$Y/${f##*/}" | cut -d' ' -f1)" ]; then
        fmt=$((fmt + 1))
      else
        say "  regenerated with a different change: ${f##*/} - check it before pushing"
      fi
    done
    [ "$fmt" -eq 0 ] || say "  $fmt file(s) differ in formatting only (zero From line, full index lines, hunk offsets)"
  )
  rc=$?
  set -e
  cleanup_work
  [ $rc -eq 0 ] || exit $rc
  say "next (confirm each push): git -C $XB push origin yacer-kodi; then yacer-kodi.sh patches"
}

case "${1:-}" in
  plan) shift; cmd_plan "$@" ;;
  push) shift; cmd_push "$@" ;;
  guard) shift; cmd_guard "$@" ;;
  import) shift; cmd_import "$@" ;;
  start) shift; cmd_start "$@" ;;
  enable) shift; cmd_enable "$@" ;;
  land) shift; cmd_land "$@" ;;
  patches) shift; cmd_patches "$@" ;;
  abandon) shift; cmd_abandon "$@" ;;
  *) die "usage: yacer-kodi.sh plan|push <plan>|guard [--local [file]]|import [<ce> [<yacer>]]|start [--upstream]|enable <file>|land -m <msg>|patches|abandon" ;;
esac
