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
# The data links (setup/data-links.txt) of the tree in use, made when its first is missing and the data is here: a
# lane's copy (a git clone has none) reads and writes the same data as main's through its own games' workspaces
# (paths.env: AI_WORKSPACE, DM2_WORKSPACE are ZAXIS's). Only missing links to data that exists, in a folder the copy
# has (games/<game>, tools/workdb); nothing else is changed (setup.sh does the rest: --check, the data folders it makes). zpaths.py the same.
if [ -f "$ZAXIS_TOOLING/setup/data-links.txt" ] && [ ! -L "$AI_WORKSPACE/recomp/generated" ] && [ -e "$AI_DATA/recomp/generated" ]; then
  while read -r _zp_path _zp_var _zp_rel || [ -n "$_zp_path" ]; do
    case $_zp_path in ''|'#'*) continue ;; esac
    eval "_zp_data=\${$_zp_var:-}"
    _zp_link=$ZAXIS/$_zp_path
    _zp_game=${_zp_path#*/}; _zp_game=$ZAXIS/${_zp_path%%/*}/${_zp_game%%/*}   # (games/<game>, tools/workdb)
    if [ -n "$_zp_data" ] && [ -d "$_zp_game" ] && [ ! -e "$_zp_link" ] && [ ! -L "$_zp_link" ] && [ -e "$_zp_data/$_zp_rel" ]; then
      mkdir -p "${_zp_link%/*}" && ln -s "$_zp_data/$_zp_rel" "$_zp_link" 2>/dev/null
    fi
  done < "$ZAXIS_TOOLING/setup/data-links.txt"
fi
unset _zp_here _zp_file _zp_common _zp_line _zp_name _zp_set _zp_value _zp_home _zp_path _zp_var _zp_rel _zp_data _zp_link _zp_game
# zguard TOOL [PATH ...]: this script stopped (exit 2, a message saying why) when the working directory or an output
# PATH resolves inside the main checkout, unless the orchestrator's merge and batch scripts run it (ZAXIS_IN_MAIN=1,
# which round-merge.sh, batchS.sh, merge-check.sh and push-batch.sh export). zguard.py says what counts (tools36).
zguard() {
  [ "${ZAXIS_IN_MAIN:-}" = 1 ] && return 0
  [ -f "$ZAXIS_TOOLING/zguard.py" ] || return 0   # (a worker's partial copy: no main checkout there)
  python3 -I "$ZAXIS_TOOLING/zguard.py" "$@" || exit 2
}
# Test runs make no sound (mute1): the runner opens SDL's dummy audio device, everything before the speakers as with
# sound (ps2/runtime/host_sdl.cpp, docs/DEVTOOLS.md). A run that has to be heard says ZX_AUDIO=on itself.
export ZX_AUDIO=${ZX_AUDIO:-off}

# zbind TREE: the bind tables a build of TREE uses (zpaths.bind_dir): its own tools/readable/bind, committed since
# tools39, else the shared $GAME_BIND_LEGACY of a commit from before. paths.env has no GAME_BIND_DIR: a script names the
# tree it builds or reads (lanecheck.sh, merge-check.sh, mac/gate.sh, make-app.sh).
# zgen FOLDER: a game's generated folder (the workspace's recomp/generated, recomp/generated-ns, or BMX_DATA's) as this
# tooling's builds use it: FOLDER's real path with -$GENERATED_SET after it (paths.env), or FOLDER when the set is empty.
zgen() {
  if [ -n "${GENERATED_SET:-}" ]; then printf '%s-%s\n' "$(cd "$1" && pwd -P)" "$GENERATED_SET"; else printf '%s\n' "$1"; fi
}
zbind() {
  if [ -f "$1/tools/readable/bind/dm2.tsv" ]; then printf '%s\n' "$1/tools/readable/bind"; else printf '%s\n' "$GAME_BIND_LEGACY"; fi
}
# zdir TREE KIND: that top-level folder's name in TREE (engine, ps2, games, collection, platform, tools, recompiler,
# iop, third_party, tests, docs; Python: checks/zlayout.py). A tree from before layout1 is not read.
zdir() {
  case $2 in engine|ps2|games|collection|platform|tools|recompiler|iop|third_party|tests|docs) printf '%s\n' "$2" ;;
    *) echo "zdir: no folder kind $2" >&2; return 2 ;; esac
}
# zruninputs: what a game run's result depends on, as git pathspecs (one a line): the program's sources, the tables and
# generators its build runs and the C++ of the tools it builds (zx-fncheck's main among them); not the games' tools,
# docs and workspaces, the tools' scripts, the docs or the workshop. Two commits that agree on these give the same game
# runs (merge-check.sh and batchS.sh carry the integration lane's then).
zruninputs() {
  printf '%s\n' CMakeLists.txt engine ps2 iop third_party collection platform games tools/readable tools/setup \
    ':(glob)tools/checks/*.cpp' ':(glob)tools/modding/*.cpp' \
    ':(exclude,glob)games/*/tools/**' ':(exclude,glob)games/*/docs/**' ':(exclude,glob)games/*/workspace/**'
}
# ztooling: what was the tooling's before layout1 (its scripts, the games' workspaces, the recompiler), as git pathspecs
# (one a line): the files the orchestrator's merge applies lanes' old patches to and push-batch.sh commits with the
# workshop's.
ztooling() {
  printf '%s\n' tools recompiler ':(glob)games/*/tools/**' ':(glob)games/*/workspace/**' ':(glob)games/*/docs/**' \
    ':(glob)platform/**/*.sh' ':(glob)platform/**/*.ps1' ':(glob)platform/**/*.swift'
}
# zreadable TREE: the readable code's seven folders in TREE, one a line in key order (checks/zlayout.py READABLE:
# a game's own folder holds its overrides/, tools/, docs/ and workspace/ beside the code).
zreadable() {
  printf '%s\n' "$1/games/aggressive" "$1/games/bmx" "$1/games/dm2" "$1/ps2/ee" "$1/engine/host" "$1/ps2/sce" "$1/engine/zge"
}
