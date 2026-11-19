#!/bin/bash
# Ghidra on a Z-Axis Xbox build (default.xbe), in a scratch folder: the Xbox counterpart of
# pipeline.sh, for tools/xmatch.py (MATCH3.md "The Xbox build").
# Usage: tools/ghidra/xbe_pipeline.sh <name> <default.xbe copy> <work dir> [first_stage]
#   1 import (XboxDev ghidra-xbe's XBE loader) and full analysis: x86 auto-analysis, the
#     loader's XbSymbolDatabase and XTLID analyzers (XDK names), Function ID (MSVC libraries);
#     "Non-Returning Functions - Discovered" off (as in the PS2 pipeline: it marks functions that
#     do return, cutting their callers short), "Decompiler Parameter ID" on (LTCG passes
#     arguments in registers; this finds them)
#   2 gap fill (XbeGapFill.java): functions reached only through pointers in data, and code
#     after alignment padding between functions, with re-analysis after each round
#   3 export: <work>/out/<name>-xbe.jsonl (functions, calls, data refs, immediates,
#     displacements, strings: ExportXbe.java)
#   4 full decompiler output: <work>/<name>-decomp.c (ExportDecomp.java)
#   5 with READABLE_BUILD set: the readable write-back (readable_pass.sh: <work>/readable/)
# Needs the ghidra-xbe release for this Ghidra (github.com/XboxDev/ghidra-xbe, the
# ghidra_12.1.4_PUBLIC_*_ghidra-xbe.zip asset) at $XBE_EXT_ZIP; it is unpacked into the work
# dir's own Ghidra settings (nothing outside <work> changes). The release ships
# XbSymbolDatabaseCLI for mac_x86_64 only; on Apple silicon it is copied to mac_arm_64 too and
# runs under Rosetta. Outputs are derived from the disc: never commit them.
set -euo pipefail
NAME=$1; XBE=$(cd "$(dirname "$2")" && pwd)/$(basename "$2"); G=$3; FIRST=${4:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
XBE_EXT_ZIP=${XBE_EXT_ZIP:?set XBE_EXT_ZIP to the ghidra-xbe release zip}
PROG=$(basename "$XBE")
mkdir -p "$G"/{scripts,logs,settings,out,project}
AI_SCRIPTS=${AI_SCRIPTS:-$(cd "$(dirname "$0")/../../../ai/ghidra/scripts" && pwd -P)}
cp "$HERE"/ExportDecomp.java "$HERE"/ExportXbe.java "$HERE"/XbeGapFill.java "$HERE"/XbeDump.java \
  "$AI_SCRIPTS"/SetAnalyzerOption.java "$G/scripts/"
for d in ghidra "$(id -un)-ghidra"; do
  X=$G/settings/$d/ghidra_12.1.4_PUBLIC/Extensions
  mkdir -p "$X"
  if [ ! -d "$X/ghidra-xbe" ]; then
    unzip -q -o "$XBE_EXT_ZIP" -d "$X"
    chmod +x "$X"/ghidra-xbe/os/*/XbSymbolDatabaseCLI
    if [ "$(uname -s)-$(uname -m)" = Darwin-arm64 ]; then
      mkdir -p "$X/ghidra-xbe/os/mac_arm_64"
      cp "$X/ghidra-xbe/os/mac_x86_64/XbSymbolDatabaseCLI" "$X/ghidra-xbe/os/mac_arm_64/"
    fi
  fi
done
RUN() {
  JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21} \
  GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-6G} \
  GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$G/settings" \
  "$(brew --prefix ghidra)/libexec/support/analyzeHeadless" "$G/project" "$NAME" "$@"
}
S="-scriptPath $G/scripts"
cd "$G"

stage() { # stage <n> <name> <args...>
  local n=$1 name=$2; shift 2
  if [ "$n" -lt "$FIRST" ]; then return 0; fi
  local log=$G/logs/xbe-$(printf %02d "$n")-$name.log
  echo "[$(date +%T)] stage $n $name -> $log"
  if ! "$@" > "$log" 2>&1; then echo "stage $n failed, see $log"; exit 1; fi
  if grep -q -E 'SCRIPT ERROR|Exception in thread|GhidraScriptLoadException' "$log"; then
    echo "stage $n: script error, see $log"; exit 1
  fi
  grep -E '(analysis option|ExportXbe wrote|ExportDecomp wrote|XbeGapFill|REPORT: (Save|Import|Analysis)|Xbox|XbSymbol|Failed to find)' "$log" \
    | sed 's/^INFO  //; s/ (HeadlessAnalyzer) *$//' || true
}

if [ "$FIRST" -le 1 ]; then rm -rf "$G/project" && mkdir -p "$G/project"; fi
stage 1 import RUN -import "$XBE" -overwrite -loader XbeLoader -analysisTimeoutPerFile 7200 $S \
  -preScript SetAnalyzerOption.java "Non-Returning Functions - Discovered=false" "Decompiler Parameter ID=true"
stage 2 gapfill RUN -process "$PROG" -noanalysis $S -postScript XbeGapFill.java
stage 3 export RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportXbe.java "$G/out/$NAME-xbe.jsonl"
stage 4 decomp RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportDecomp.java "$G/$NAME-decomp.c"
# 5. the readable write-back (MATCH3.md section 7), when READABLE_BUILD names this build in the
#    work database (ai_ps2, dm2_ps2, bmx_ps2, ai_xbox, ai_gc): on a copy, <work>/project-readable
if [ -n "${READABLE_BUILD:-}" ] && [ "$FIRST" -le 5 ]; then
  echo "[$(date +%T)] stage 5 readable ($READABLE_BUILD)"
  python3 -I "$HERE/../readable_ghidra.py" gen "$READABLE_BUILD" "$G/readable"
  "$HERE/readable_pass.sh" xbe "$NAME" "$G" "$PROG" "$G/readable/readable-$READABLE_BUILD.jsonl"
fi
echo "[$(date +%T)] done"
