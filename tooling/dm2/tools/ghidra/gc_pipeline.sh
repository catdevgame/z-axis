#!/bin/bash
# Ghidra on a Z-Axis GameCube build (main.dol: PowerPC Gekko, CodeWarrior, no symbols), in a
# scratch folder: the GameCube counterpart of xbe_pipeline.sh, for tools/gcmatch.py (MATCH3.md
# section 5).
# Usage: tools/ghidra/gc_pipeline.sh <name> <main.dol copy> <work dir> [first_stage]
#   1 import (-loader-autoloadMaps false: the loader would ask for a symbol map in a dialog)
#     with Cuyler36's Ghidra-GameCube-Loader (DOL loader, the Gekko/Broadway language
#     with paired singles; its "(GameCube/Wii) Program Analyzer" finds the r2/r13 small-data
#     bases and resolves r2/r13-relative references; turned on here, as its default needs an
#     executable format name this Ghidra does not give the DOL), full analysis;
#     "Non-Returning Functions - Discovered" off (as in the other pipelines)
#   2 export: <work>/out/<name>-gc.jsonl (ExportXbe.java with PowerPC's stack and small-data
#     registers left out of the displacements)
#   3 full decompiler output: <work>/<name>-decomp.c (ExportDecomp.java)
#   4 with READABLE_BUILD set: the readable write-back (readable_pass.sh: <work>/readable/)
# Needs the loader's release zip for this Ghidra (github.com/Cuyler36/Ghidra-GameCube-Loader,
# release 1.3.1, GameCubeLoader-1.3.0-921504c-Ghidra_12.1.zip) at $GC_EXT_ZIP. It is unpacked
# into the work dir's own Ghidra settings (nothing outside <work> changes), its
# extension.properties version set to this Ghidra's (12.1.4; the zip says 12.1) and its SLEIGH
# language compiled there with Ghidra's own support/sleigh. Outputs are derived from the disc:
# never commit them.
set -euo pipefail
NAME=$1; DOL=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); G=$3; FIRST=${4:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
GC_EXT_ZIP=${GC_EXT_ZIP:?set GC_EXT_ZIP to the Ghidra-GameCube-Loader release zip}
GH=$(brew --prefix ghidra)/libexec
VER=$(sed -n 's/^application.version=//p' "$GH/Ghidra/application.properties")
PROG=$(basename "$DOL")
mkdir -p "$G"/{scripts,logs,settings,out,project}
AI_SCRIPTS=${AI_SCRIPTS:-$(cd "$(dirname "$0")/../../../ai/ghidra/scripts" && pwd -P)}
cp "$HERE"/ExportDecomp.java "$HERE"/ExportXbe.java "$AI_SCRIPTS"/SetAnalyzerOption.java "$G/scripts/"
for d in ghidra "$(id -un)-ghidra"; do
  X=$G/settings/$d/ghidra_${VER}_PUBLIC/Extensions
  mkdir -p "$X"
  if [ ! -d "$X/GameCubeLoader" ]; then
    unzip -q -o "$GC_EXT_ZIP" -d "$X"
    sed -i '' "s/^version=.*/version=$VER/" "$X/GameCubeLoader/extension.properties"
    L=$X/GameCubeLoader/data/languages
    # the language uses PowerPC's shared .sinc files: compile it beside Ghidra's own
    ( cd "$L" && for f in "$GH"/Ghidra/Processors/PowerPC/data/languages/*.sinc; do
        [ -e "$(basename "$f")" ] || ln -s "$f" .; done
      JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21} "$GH/support/sleigh" ppc_gekko_broadway.slaspec \
        > "$G/logs/sleigh-$d.log" 2>&1 )
  fi
done
RUN() {
  JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21} \
  GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-6G} \
  GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$G/settings" \
  "$GH/support/analyzeHeadless" "$G/project" "$NAME" "$@"
}
S="-scriptPath $G/scripts"
cd "$G"

stage() { # stage <n> <name> <args...>
  local n=$1 name=$2; shift 2
  if [ "$n" -lt "$FIRST" ]; then return 0; fi
  local log=$G/logs/gc-$(printf %02d "$n")-$name.log
  echo "[$(date +%T)] stage $n $name -> $log"
  if ! "$@" > "$log" 2>&1; then echo "stage $n failed, see $log"; exit 1; fi
  if grep -q -E 'SCRIPT ERROR|Exception in thread|GhidraScriptLoadException' "$log"; then
    echo "stage $n: script error, see $log"; exit 1
  fi
  grep -E '(analysis option|ExportXbe wrote|ExportDecomp wrote|REPORT: (Save|Import|Analysis)|Using Loader|SDA|GameCube|Failed)' "$log" \
    | sed 's/^INFO  //; s/ (HeadlessAnalyzer) *$//' || true
}

if [ "$FIRST" -le 1 ]; then rm -rf "$G/project" && mkdir -p "$G/project"; fi
stage 1 import RUN -import "$DOL" -overwrite -loader GameCubeLoader -loader-autoloadMaps false -analysisTimeoutPerFile 7200 $S \
  -preScript SetAnalyzerOption.java "Non-Returning Functions - Discovered=false" "(GameCube/Wii) Program Analyzer=true"
stage 2 export RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportXbe.java "$G/out/$NAME-gc.jsonl" r1,r2,r13
stage 3 decomp RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportDecomp.java "$G/$NAME-decomp.c"
# 4. the readable write-back (MATCH3.md section 7), when READABLE_BUILD names this build in the
#    work database (ai_ps2, dm2_ps2, bmx_ps2, ai_xbox, ai_gc): on a copy, <work>/project-readable
if [ -n "${READABLE_BUILD:-}" ] && [ "$FIRST" -le 4 ]; then
  echo "[$(date +%T)] stage 4 readable ($READABLE_BUILD)"
  python3 -I "$HERE/../readable_ghidra.py" gen "$READABLE_BUILD" "$G/readable"
  "$HERE/readable_pass.sh" gc "$NAME" "$G" "$PROG" "$G/readable/readable-$READABLE_BUILD.jsonl"
fi
echo "[$(date +%T)] done"
