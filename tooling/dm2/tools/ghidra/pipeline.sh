#!/bin/bash
# The carry-over study's Ghidra pipeline for any of the engine's PS2 executables
# (AI SLUS_203.27, DM2 SLUS_201.59, BMX XXX SLUS_204.15), in a scratch folder.
# Usage: tools/ghidra/pipeline.sh <name> <elf copy> <work dir> [first_stage]
#   stages 1..8 as in the DM2 pipeline, then 9 = full decompiler output (<work>/<name>-decomp.c),
#   10 = the readable write-back with READABLE_BUILD set (readable_pass.sh: <work>/readable/).
# The Ghidra scripts are the AI pipeline's (tooling/ai/ghidra/scripts, read
# only, copied into <work>/scripts) plus ExportDecomp.java from this folder. Outputs (all
# disc-derived, never commit): <work>/out/ghidra-functions.csv, <work>/out/config.toml,
# <work>/reports/, <work>/sce-db/sce-matches.csv, <work>/<name>-decomp.c.
set -euo pipefail
NAME=$1; ELF=$2; G=$3; FIRST=${4:-1}
AI_SCRIPTS=${AI_SCRIPTS:-$(cd "$(dirname "$0")/../../../ai/ghidra/scripts" && pwd -P)}
SCEDB_H=${SCEDB_H:-$AI_SCRIPTS/../../../recompiler/ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h}
HERE=$(cd "$(dirname "$0")" && pwd)
PROG=$(basename "$ELF")
mkdir -p "$G"/{scripts,logs,reports,sce-db,settings,out,project}
cp "$AI_SCRIPTS"/*.java "$AI_SCRIPTS"/sce_scan.py "$G/scripts/"
cp "$HERE"/ExportDecomp.java "$G/scripts/"
# the ghidra-emotionengine-reloaded extension (r5900 language), from the AI project's local settings;
# Ghidra names the settings subfolder "ghidra" or "<user>-ghidra" depending on the folder, so both
EXT=${EE_EXT:-$AI_SCRIPTS/../settings/ghidra/ghidra_12.1.4_PUBLIC/Extensions/ghidra-emotionengine-reloaded}
for d in ghidra "$(id -un)-ghidra"; do
  mkdir -p "$G/settings/$d/ghidra_12.1.4_PUBLIC/Extensions"
  [ -d "$G/settings/$d/ghidra_12.1.4_PUBLIC/Extensions/$(basename "$EXT")" ] || cp -R "$EXT" "$G/settings/$d/ghidra_12.1.4_PUBLIC/Extensions/"
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
  local log=$G/logs/pipeline-$(printf %02d "$n")-$name.log
  echo "[$(date +%T)] stage $n $name -> $log"
  if ! "$@" > "$log" 2>&1; then echo "stage $n failed, see $log"; exit 1; fi
  if grep -q -E 'SCRIPT ERROR|REPORT SCRIPT ERROR|Exception in thread|GhidraScriptLoadException' "$log"; then
    echo "stage $n: script error, see $log"; exit 1
  fi
  grep -E '(analysis option|GapFill done|FixSwitchTables:|FixNoReturn:|FixDetachedChunks:|MergeFragments:|Applied [0-9]+ SCE|Exported|ExportDecomp wrote|REPORT: (Save|Import|Analysis))' "$log" \
    | sed 's/^INFO  //; s/ (GhidraScript) *$//; s/ (HeadlessAnalyzer) *$//' || true
}

# 0. SCE SDK signature scan (pure Python)
if [ "$FIRST" -le 1 ]; then
  [ -f "$G/sce-db/symbols.json" ] || python3 -I "$G/scripts/sce_scan.py" extract "$SCEDB_H" "$G/sce-db"
  [ -f "$G/sce-db/sce-matches.csv" ] || python3 -I "$G/scripts/sce_scan.py" scan "$G/sce-db" "$ELF" "$G/sce-db/sce-matches.csv"
  rm -rf "$G/project" && mkdir -p "$G/project"
fi
# 1. import with the R5900 language; execute cleared on every block except .text
stage 1 import RUN -import "$ELF" -overwrite -noanalysis -processor r5900:LE:32:default -cspec default \
  $S -preScript PrepareEEProgram.java
# 2. SCE names as entry points, full auto-analysis ("Shared Return Calls" off, see the DM2 pipeline)
stage 2 analyze RUN -process "$PROG" $S -preScript SetAnalyzerOption.java "Shared Return Calls=false" \
  -preScript ApplySceSymbols.java "$G/sce-db/sce-matches.csv" -analysisTimeoutPerFile 7200
NORET_OFF="Non-Returning Functions - Discovered=false"
# 3. bogus noreturn + jump tables, re-analyse
stage 3 fix-flow RUN -process "$PROG" $S -preScript SetAnalyzerOption.java "$NORET_OFF" \
  -preScript FixNoReturn.java -preScript FixSwitchTables.java
# 4. pointer-only functions in the gaps
stage 4 gapfill RUN -process "$PROG" $S -preScript FixNoReturn.java -preScript GapFill.java
# 5. second jump-table round, remote shared epilogues
stage 5 cleanup RUN -process "$PROG" $S -preScript FixNoReturn.java -preScript FixSwitchTables.java \
  -preScript MergeFragments.java -preScript GapFill.java -preScript FixDetachedChunks.java
# 6. verification (dry runs must report nothing left)
stage 6 verify RUN -process "$PROG" -noanalysis -readOnly $S -postScript FixNoReturn.java dry \
  -postScript FixSwitchTables.java dry -postScript MergeFragments.java dry -postScript GapFill.java dry -postScript FixDetachedChunks.java dry
# 7. quality report + source-file grouping
stage 7 report RUN -process "$PROG" -noanalysis -readOnly $S -postScript MapQualityReport.java "$G/reports"
# 8. PS2Recomp export (CSV + TOML)
stage 8 export RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportPS2FunctionsHeadless.java \
  "$G/out/config.toml" "$G/out/ghidra-functions.csv" "$ELF" "$G/out/generated"
# 9. full decompiler output
stage 9 decomp RUN -process "$PROG" -noanalysis -readOnly $S -postScript ExportDecomp.java "$G/$NAME-decomp.c"
# 10. the readable write-back (MATCH3.md section 7), when READABLE_BUILD names this build in the
#    work database (ai_ps2, dm2_ps2, bmx_ps2, ai_xbox, ai_gc): on a copy, <work>/project-readable
if [ -n "${READABLE_BUILD:-}" ] && [ "$FIRST" -le 10 ]; then
  echo "[$(date +%T)] stage 10 readable ($READABLE_BUILD)"
  python3 -I "$HERE/../readable_ghidra.py" gen "$READABLE_BUILD" "$G/readable"
  "$HERE/readable_pass.sh" ps2 "$NAME" "$G" "$PROG" "$G/readable/readable-$READABLE_BUILD.jsonl"
fi
echo "[$(date +%T)] done"
