#!/bin/bash
# The readable stage of the Ghidra pipelines (MATCH3.md section 7): the readable rewrite's names,
# signatures, structs and typed globals written into a copy of an analysed project, read back, and
# the exports made again from it.
# Usage: tools/workdb/ghidra/readable_pass.sh <kind> <name> <work dir> <program> <readable-BUILD.jsonl> [first_stage]
#   kind: ps2 (pipeline.sh), xbe (xbe_pipeline.sh) or gc (gc_pipeline.sh): which export the
#   copy gets besides the decompilation; name, work dir: the pipeline's; program: the program
#   in its project (the ELF/XBE/DOL file name). The jsonl (and the readable-BUILD.h beside it) come
#   from `tools/workdb/readable_ghidra.py gen BUILD DIR`.
#   1 copy: <work>/project -> <work>/project-readable (the base project stays as the analysis
#     left it: its exports feed the recompiler and the pairings)
#   2 apply: ApplyReadable.java (fails on any read-back mismatch); records the input's SHA-256
#     in the program's Program Information
#   3 verify and export, read only: ApplyReadable.java verify (the saved program against the
#     file), the export (xbe/gc: <work>/readable/<name>-xbe.jsonl or -gc.jsonl, with the analysis
#     names as "n" and the readable ones as "rn"), the decompilation <work>/readable/<name>-decomp.c
# The work dir's settings (with the pipeline's extension) are used as they are; the scripts are
# copied to <work>/readable/scripts (ApplyReadable.java from the AI pipeline's scripts, $AI_SCRIPTS). Outputs are
# derived from the disc: never commit them.
set -euo pipefail
KIND=$1; NAME=$2; G=$(cd "$3" && pwd); PROG=$4; IN=$(cd "$(dirname "$5")" && pwd)/$(basename "$5"); FIRST=${6:-1}
HERE=$(cd "$(dirname "$0")" && pwd)
AI_SCRIPTS=${AI_SCRIPTS:-$(cd "$(dirname "$0")/../../../games/aggressive/workspace/ghidra/scripts" && pwd -P)}
mkdir -p "$G"/{logs,readable/scripts}
cp "$AI_SCRIPTS"/ApplyReadable.java "$HERE"/ExportDecomp.java "$HERE"/ExportXbe.java "$G/readable/scripts/"
RUN() {
  JAVA_HOME=${JAVA_HOME:-/opt/homebrew/opt/openjdk@21} \
  GHIDRA_HEADLESS_MAXMEM=${GHIDRA_HEADLESS_MAXMEM:-6G} \
  GHIDRA_HEADLESS_JAVA_OPTIONS="-Dapplication.settingsdir=$G/settings" \
  "$(brew --prefix ghidra)/libexec/support/analyzeHeadless" "$G/project-readable" "$NAME" "$@"
}
S="-scriptPath $G/readable/scripts"
cd "$G"
stage() { # stage <n> <name> <args...>
  local n=$1 name=$2; shift 2
  if [ "$n" -lt "$FIRST" ]; then return 0; fi
  local log=$G/logs/readable-$(printf %02d "$n")-$name.log
  echo "[$(date +%T)] readable stage $n $name -> $log"
  if ! "$@" > "$log" 2>&1; then echo "readable stage $n failed, see $log"; exit 1; fi
  if grep -q -E 'SCRIPT ERROR|Exception in thread|GhidraScriptLoadException|ApplyReadable mismatch' "$log"; then
    echo "readable stage $n: script error or mismatch, see $log"; exit 1
  fi
  grep -E '(ApplyReadable (applied|verified|parser)|ExportXbe wrote|ExportDecomp wrote|REPORT: Save)' "$log" \
    | sed 's/^INFO  //; s/ (GhidraScript) *$//; s/ (HeadlessAnalyzer) *$//' || true
}
copy() { rm -rf "$G/project-readable" && cp -R "$G/project" "$G/project-readable"; }
EXPORT=()
case "$KIND" in
  ps2) ;;
  xbe) EXPORT=(-postScript ExportXbe.java "$G/readable/$NAME-xbe.jsonl") ;;
  gc) EXPORT=(-postScript ExportXbe.java "$G/readable/$NAME-gc.jsonl" r1,r2,r13) ;;
  *) echo "kind: ps2, xbe or gc"; exit 2 ;;
esac
stage 1 copy copy
stage 2 apply RUN -process "$PROG" -noanalysis $S -postScript ApplyReadable.java "$IN"
stage 3 export RUN -process "$PROG" -noanalysis -readOnly $S -postScript ApplyReadable.java "$IN" verify \
  ${EXPORT[@]+"${EXPORT[@]}"} -postScript ExportDecomp.java "$G/readable/$NAME-decomp.c"
echo "[$(date +%T)] readable done"
