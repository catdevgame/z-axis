# paths.sh: source it (bash or zsh) for the z-axis layout as exported variables: every name in
# paths.env, unless already set, plus ZAXIS_TOOLING (this folder), ZAXIS (the checkout it is in) and, in the
# workshop's checkout, ZAXIS_WORKSHOP (its workshop/ folder: merge, remote, status; not in the public repository).
if [ -n "$ZSH_VERSION" ]; then _zp_here=${${(%):-%x}:A:h}; else _zp_here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd -P); fi
export ZAXIS_TOOLING=${ZAXIS_TOOLING:-$_zp_here}
export ZAXIS=${ZAXIS:-$(cd "$_zp_here/.." && pwd -P)}
if [ -z "${ZAXIS_MAIN:-}" ]; then
  _zp_common=$(git -C "$ZAXIS" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)
  case $_zp_common in */.git) export ZAXIS_MAIN=${_zp_common%/.git} ;; esac
fi
# (paths.env's $HOME is the user's own home, or ZAXIS_HOME: never the environment's HOME, so a scratch HOME still
# finds the data; zpaths.py the same)
_zp_home=${ZAXIS_HOME:-$(eval echo "~$(id -un)")}
# The workshop: this checkout's workshop/, else the main checkout's (a tooling copy outside a checkout); none in a
# public checkout.
if [ -z "${ZAXIS_WORKSHOP:-}" ]; then
  if [ -d "$ZAXIS/workshop/merge" ]; then export ZAXIS_WORKSHOP=$ZAXIS/workshop
  elif [ -n "${ZAXIS_MAIN:-}" ] && [ -d "$ZAXIS_MAIN/workshop/merge" ]; then export ZAXIS_WORKSHOP=$ZAXIS_MAIN/workshop; fi
fi
# The workshop's layout first (workshop/merge/paths.env, when there), then paths.env's defaults: the first value of a
# name wins.
for _zp_file in "${ZAXIS_WORKSHOP:-$_zp_here}/merge/paths.env" "$_zp_here/paths.env"; do
  [ -f "$_zp_file" ] || continue
  while IFS= read -r _zp_line || [ -n "$_zp_line" ]; do
    case $_zp_line in ''|'#'*) continue ;; esac
    _zp_name=${_zp_line%%=*}
    _zp_value=${_zp_line#*=}; _zp_value=${_zp_value//\$HOME/$_zp_home}
    eval "_zp_set=\${$_zp_name:-}"
    [ -n "$_zp_set" ] || eval "export $_zp_name=\"$_zp_value\""
  done < "$_zp_file"
done
# The data links (data-links.txt) of the tooling in use, made when its first is missing and the data is here: a lane's
# tooling copy (a git clone has none) reads and writes the same data as main's through its own workspaces (paths.env:
# AI_WORKSPACE, DM2_WORKSPACE are ZAXIS_TOOLING's). Only missing links to data that exists; nothing else is changed
# (setup.sh does the rest: --check, the data folders it makes). zpaths.py the same.
if [ -f "$ZAXIS_TOOLING/data-links.txt" ] && [ ! -L "$ZAXIS_TOOLING/ai/recomp/generated" ] && [ -e "$AI_DATA/recomp/generated" ]; then
  while read -r _zp_ws _zp_rel || [ -n "$_zp_ws" ]; do
    case $_zp_ws in ai) _zp_data=$AI_DATA ;; dm2) _zp_data=$DM2_DATA ;; *) continue ;; esac
    _zp_link=$ZAXIS_TOOLING/$_zp_ws/$_zp_rel
    if [ -d "$ZAXIS_TOOLING/$_zp_ws/tools" ] && [ ! -e "$_zp_link" ] && [ ! -L "$_zp_link" ] && [ -e "$_zp_data/$_zp_rel" ]; then
      mkdir -p "${_zp_link%/*}" && ln -s "$_zp_data/$_zp_rel" "$_zp_link" 2>/dev/null
    fi
  done < "$ZAXIS_TOOLING/data-links.txt"
fi
unset _zp_here _zp_file _zp_common _zp_line _zp_name _zp_set _zp_value _zp_home _zp_ws _zp_rel _zp_data _zp_link
# zguard TOOL [PATH ...]: this script stopped (exit 2, a message saying why) when the working directory or an output
# PATH resolves inside the main checkout, unless the orchestrator's merge and batch scripts run it (ZAXIS_IN_MAIN=1,
# which round-merge.sh, batchS.sh, merge-check.sh and push-batch.sh export). zguard.py says what counts (tools36).
zguard() {
  [ "${ZAXIS_IN_MAIN:-}" = 1 ] && return 0
  [ -f "$ZAXIS_TOOLING/zguard.py" ] || return 0   # (a worker's partial copy: no main checkout there)
  python3 -I "$ZAXIS_TOOLING/zguard.py" "$@" || exit 2
}
# Test runs make no sound (mute1): the runner opens SDL's dummy audio device, everything before the speakers as with
# sound (runtime/src/lib/ps2_host_sdl.cpp, docs/DEVTOOLS.md). A run that has to be heard says ZX_AUDIO=on itself.
export ZX_AUDIO=${ZX_AUDIO:-off}

# zbind TREE: the bind tables a build of TREE uses (zpaths.bind_dir): its own runtime/tools/bind, committed since
# tools39, else the shared $GAME_BIND_LEGACY of a commit from before. paths.env has no GAME_BIND_DIR: a script names the
# tree it builds or reads (lanecheck.sh, merge-check.sh, mac/gate.sh, make-app.sh).
# zgen FOLDER: a game's generated folder (the workspace's recomp/generated, recomp/generated-ns, or BMX_DATA's) as this
# tooling's builds use it: FOLDER's real path with -$GENERATED_SET after it (paths.env), or FOLDER when the set is empty.
zgen() {
  if [ -n "${GENERATED_SET:-}" ]; then printf '%s-%s\n' "$(cd "$1" && pwd -P)" "$GENERATED_SET"; else printf '%s\n' "$1"; fi
}
zbind() {
  if [ -f "$1/runtime/tools/bind/dm2.tsv" ]; then printf '%s\n' "$1/runtime/tools/bind"; else printf '%s\n' "$GAME_BIND_LEGACY"; fi
}
# zdir TREE runtime|tests|iop|docs: that top-level folder's name in TREE (Python: checks/zlayout.py). A tree from
# before tools17 (runtime, tests, ps2xIOP, recomp) is not read since naming1.
zdir() {
  case $2 in runtime|tests|iop|docs) printf '%s\n' "$2" ;; *) echo "zdir: no folder kind $2" >&2; return 2 ;; esac
}
