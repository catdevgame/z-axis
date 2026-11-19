#!/bin/zsh
# vu1_corpus_check.sh CHECKER [DIR|FILE ...]: every VU1 run corpus (*.vu1r) replayed by ps2xVu1Check twice, with the
# ahead-of-time code and with PS2X_VU1_AOT=0 (the interpreter), each run compared bit for bit with its recording
# (VU1_CORPUS.md). CHECKER: a ps2xVu1Check built with the three games' VU1 AOT folders, as the collection's and the
# app's builds are (PS2X_VU1_AOT_DIR, PS2X_VU1_AOT_EXTRA_DIRS), so it holds the AOT code the app ships. Default
# corpora: the games' traces/vu1 folders, AI_WORKSPACE's, DM2_WORKSPACE's and BMX_DATA's recomp/traces/vu1 (a game
# without one is named and fails the check: a corpus gone is no pass). One line per corpus and side: runs,
# mismatching, cycles, and which code ran. The AOT side fails where a corpus's micro memory image has no AOT code in
# CHECKER ("NO AOT": it would check the interpreter twice). Exit 0 when every run of every corpus matches on both
# sides, 1 on a mismatch, a missing corpus folder, an image without AOT code or a checker failure, 2 on a usage error.
# About 30 s on the Mac for the three games' corpora (single-threaded); no game runs.
#
# vu1_corpus_check.sh --changed REPO BASE COMMIT: the files under the VU1 paths (VU1_PATHS: the interpreter, the AOT
# generator and its build step, the checker) that differ between BASE and COMMIT in REPO, one a line; exit 0 when there
# are any, 1 when none (what gate.sh asks before it runs the check).
source ${0:A:h:h}/paths.sh
setopt extendedglob
VU1_PATHS=(runtime/src/lib/vu runtime/tools/vu1_aot.cpp runtime/tools/vu1_check.cpp ':(glob)runtime/include/runtime/ps2_vu1*.h'
           runtime/cmake/vu1_aot_generate.cmake)
if [[ $1 == --changed ]]; then
  (( $# == 4 )) || { print -u2 "usage: vu1_corpus_check.sh --changed REPO BASE COMMIT"; exit 2 }
  ch=$(git -C $2 diff --name-only $3 $4 -- $VU1_PATHS) || exit 2
  [[ -n $ch ]] || exit 1
  print -r -- $ch; exit 0
fi
CHECKER=${1:?usage: vu1_corpus_check.sh CHECKER [DIR|FILE ...]}
[[ -x $CHECKER ]] || { print -u2 "vu1_corpus_check: $CHECKER is not an executable"; exit 2 }
shift
(( $# )) || set -- $AI_WORKSPACE/recomp/traces/vu1 $DM2_WORKSPACE/recomp/traces/vu1 $BMX_DATA/recomp/traces/vu1
corpora=(); bad=0
for a in "$@"; do
  if [[ -d $a ]]; then
    found=($a/*.vu1r(N))
    (( ${#found} )) && corpora+=($found) || { print "vu1_corpus_check: no corpus in $a"; bad=1 }
  elif [[ -f $a ]]; then
    corpora+=($a)
  else
    print "vu1_corpus_check: no $a"; bad=1
  fi
done
(( ${#corpora} )) || { print "vu1_corpus_check: no corpus"; exit 1 }
runs=0
for c in $corpora; do
  for side in aot int; do
    if [[ $side == aot ]]; then out=$($CHECKER $c --quiet 2>&1); else out=$(PS2X_VU1_AOT=0 $CHECKER $c --quiet 2>&1); fi
    k=$?
    # (the checker's lines per micro memory image say which code ran: "ahead-of-time code", "interpreter (...)")
    code=$(print -r -- $out | grep '^\[vu1\] micro memory' | sed 's/^\[vu1\] micro memory //' | tr '\n' ';' | sed 's/;$//')
    last=$(print -r -- $out | tail -1)
    noaot=
    [[ $side == aot ]] && print -r -- $out | grep -q '^\[vu1\] micro memory .*: interpreter' && noaot=" NO AOT"
    print -r -- "$c $side: $last${code:+ ($code)}$noaot"
    (( k )) || [[ -n $noaot ]] && bad=1
    [[ $side == aot && $last == (#b)([0-9]##)' runs, '* ]] && (( runs += match[1] ))
  done
done
if (( bad )); then
  print "vu1_corpus_check: FAILED (a mismatch, a missing corpus, an image without AOT code or a checker failure above)"
else
  print "vu1_corpus_check: ${#corpora} corpora, $runs runs, 0 mismatching (AOT and interpreter)"
fi
exit $bad
