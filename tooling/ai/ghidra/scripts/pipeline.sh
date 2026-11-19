#!/bin/bash
# Full, resumable Ghidra pipeline for SLUS_203.27 -> PS2Recomp function map.
# Usage: scripts/pipeline.sh [first_stage]   (stages 1..9; default 1 = start from scratch)
# Every stage saves the project (except read-only stages 7/8) and logs to logs/pipeline-NN-*.log,
# so after an interruption rerun from the failed stage.
set -euo pipefail
G=$(cd "$(dirname "$0")/.." && pwd -P)
AI=$(dirname "$G")
ZAXIS=$(cd "$AI/../.." && pwd -P)  # the z-axis checkout this tooling sits in
ELF=$AI/extracted/SLUS_203.27
R=$AI/recomp
RUN=$G/scripts/run-ghidra.sh
S="-scriptPath $G/scripts"
FIRST=${1:-1}
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
  grep -E '(analysis option|GapFill done|FixSwitchTables:|FixNoReturn:|FixDetachedChunks:|MergeFragments:|Applied [0-9]+ SCE|Exported|REPORT: (Save|Import|Analysis))' "$log" \
    | sed 's/^INFO  //; s/ (GhidraScript) *$//; s/ (HeadlessAnalyzer) *$//' || true
}

# 0. SCE SDK signature scan (pure Python, no Ghidra; port of PS2Recomp's sce_symbol_scanner)
if [ "$FIRST" -le 1 ]; then
  if [ ! -f "$G/sce-db/symbols.json" ]; then
    python3 -I "$G/scripts/sce_scan.py" extract "$ZAXIS/tooling/recompiler/ps2xAnalyzer/include/ps2recomp/sce_symbol_database_data.h" "$G/sce-db"
  fi
  [ -f "$G/sce-db/sce-matches.csv" ] || python3 -I "$G/scripts/sce_scan.py" scan "$G/sce-db" "$ELF" "$G/sce-db/sce-matches.csv"
  rm -rf "$G/project" && mkdir -p "$G/project"
fi

# 1. import with the R5900 language; clear execute on every block except .text (VU code etc.)
stage 1 import $RUN -import "$ELF" -overwrite -noanalysis -processor r5900:LE:32:default -cspec default \
  $S -preScript PrepareEEProgram.java
# 2. apply SCE names as entry points, then full auto-analysis. "Shared Return Calls" is off:
#    it turned -O0 shared epilogues / loop heads reached by "b" into call+return targets and
#    split their functions into fragments (e.g. around 0x166b88).
stage 2 analyze $RUN -process SLUS_203.27 $S -preScript SetAnalyzerOption.java "Shared Return Calls=false" \
  -preScript ApplySceSymbols.java "$G/sce-db/sce-matches.csv" \
  -analysisTimeoutPerFile 7200
# 3. repair bogus noreturn + complete jump tables, then re-analyze. From here on the
#    "Non-Returning Functions - Discovered" analyzer is off: on re-analysis it wrongly flagged
#    FUN_001672d8 (which ends in jr ra) and fragmented its -O0 callers around 0x166b88.
NORET_OFF="Non-Returning Functions - Discovered=false"
stage 3 fix-flow $RUN -process SLUS_203.27 $S -preScript SetAnalyzerOption.java "$NORET_OFF" \
  -preScript FixNoReturn.java -preScript FixSwitchTables.java
# 4. recover pointer-only functions in the gaps, then re-analyze
stage 4 gapfill $RUN -process SLUS_203.27 $S -preScript FixNoReturn.java -preScript GapFill.java
# 5. second round of jump-table cleanup + detach remote shared epilogues, then re-analyze
stage 5 cleanup $RUN -process SLUS_203.27 $S -preScript FixNoReturn.java -preScript FixSwitchTables.java \
  -preScript MergeFragments.java -preScript GapFill.java -preScript FixDetachedChunks.java
# 6. verification pass (dry runs must report nothing left to do)
stage 6 verify $RUN -process SLUS_203.27 -noanalysis -readOnly $S -postScript FixNoReturn.java dry \
  -postScript FixSwitchTables.java dry -postScript MergeFragments.java dry -postScript GapFill.java dry -postScript FixDetachedChunks.java dry
# 7. quality report + source-file grouping
stage 7 report $RUN -process SLUS_203.27 -noanalysis -readOnly $S -postScript MapQualityReport.java "$G/reports"
# 8. PS2Recomp export (CSV + TOML)
stage 8 export $RUN -process SLUS_203.27 -noanalysis -readOnly $S -postScript ExportPS2FunctionsHeadless.java \
  "$R/config.toml" "$R/ghidra-functions.csv" "$ELF" "$R/generated"
# 9. the readable write-back, on a copy (project-readable; the project above stays as analysed,
#    its export names the recompiled functions): the readable rewrite's names, signatures,
#    structs and typed globals from the work database and the readable sources
#    (tooling/dm2/tools/readable_ghidra.py), applied by ApplyReadable.java and read back,
#    then the decompilation readable/AggressiveInline-decomp.c. Skipped without the DM2 repo.
DM2=${DM2_REPO:-$AI/../dm2}
if [ "$FIRST" -le 9 ]; then
  if [ -x "$DM2/tools/ghidra/readable_pass.sh" ]; then
    echo "[$(date +%T)] stage 9 readable -> $G/logs/readable-*.log"
    python3 -I "$DM2/tools/readable_ghidra.py" gen ai_ps2 "$G/readable" --src "$ZAXIS"
    AI_SCRIPTS=$G/scripts "$DM2/tools/ghidra/readable_pass.sh" ps2 AggressiveInline "$G" SLUS_203.27 \
      "$G/readable/readable-ai_ps2.jsonl"
  else
    echo "[$(date +%T)] stage 9 readable skipped (no $DM2/tools/ghidra/readable_pass.sh)"
  fi
fi
echo "[$(date +%T)] done"
