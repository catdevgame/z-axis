#!/bin/zsh
# run-game-dm2.sh <name> <max-secs> [runner]: run-game.sh (beside it) for DM2, or BMX XXX with GAME=bmx (ELF names the
# disc copy): run the game for a check, unpaced, in
# a scratch HOME, card and cwd under ${RUN_DIR:-/tmp/ai-runs}/<name>; log at
# <dir>/<name>.log. ZX_* settings pass through (pad script, readable spec,
# GS_NULL, ...).
#
# ZX_UNPACED=1 is the default: guest time follows EE cycles only, so a run
# is several times faster than real time and the logic is the same (logiccheck
# 21/21 in 34 s unpaced against 180 s paced, 2026-10-24; about 8x the ticks
# per second with GS_NULL and VU1_SKIP). Set ZX_UNPACED=0 for anything
# timed in host time: fps, frame pacing, audio, visuals.
#
# STOP_TICK=<n>: stop as soon as the game reaches vsync tick n (needs a
# ZX_STATE_LOG; one is added if missing) instead of waiting max-secs.
# max-secs stays the hard time limit, so no run is ever left behind.
# The tick is read from the state log's file (ZX_STATE_LOG_FILE, one is
# added if missing: the probe writes it whole line by line, whatever else the
# runner prints or not), and from the log as before. A stopped run is waited
# for: the script returns once the runner has exited and its files are closed.
NAME=$1; SECS=$2
RUNNER=${3:-$RUNNER}
source ${0:A:h:h}/paths.sh   # the shared environment (no sound: ZX_AUDIO)
R=$DM2_WORKSPACE/recomp
D=${RUN_DIR:-/tmp/ai-runs}/$NAME
LOG=$D/$NAME.log
[ -z "$NAME" -o -z "$SECS" ] && { echo "usage: run-game-dm2.sh <name> <max-secs> [runner]"; exit 2; }
zguard run-game-dm2.sh $D   # (not from or into the main checkout: paths.sh, tools36)
mkdir -p $D/home $D/cwd $D/app $D/shots
mkdir -p $D/mc0   # a fresh card, as DM2's routes start
export ZX_UNPACED=${ZX_UNPACED:-1}
MODE_WORD=0x259770; [ "$GAME" = bmx ] && MODE_WORD=0x29D040
[ -z "$ELF" ] && ELF=$R/disc-root/SLUS_201.59
[ "$GAME" = bmx ] && [ -z "${ELF:#$R/disc-root/SLUS_201.59}" ] && ELF=$BMX_DATA/disc/SLUS_204.15   # (paths.env: never the environment's HOME, a scratch one under a run, tools34)
[ -n "$STOP_TICK" -a -z "$ZX_STATE_LOG" ] && { export ZX_STATE_LOG=$MODE_WORD; ADDED_STATE_LOG=1; }
STATE=${ZX_STATE_LOG_FILE:-}
[ -n "$STOP_TICK" -a -z "$STATE" ] && { STATE=$D/stop-tick-state.log; rm -f $STATE; export ZX_STATE_LOG_FILE=$STATE; }
# (the state log writes a line when a word changes; the pad log one at every pad read, about every vsync once the
# game reads the pad: the mode word's, so the tick is seen whatever the state log's words do. With a caller's own state
# log too (career_ab.py's): a runner with no [frame:upload] lines (the app's flavour) on an input whose words stop
# changing (DM2's editor-0 after tick 3126, BMX XXX's frontend after 1386) never showed its tick and ran to the time
# limit, 1,200 s of flavour_ab.py's 1,360 and 1,205 in int27gate. Its [pad-state] lines are no [state] lines: the
# state log's readers (career_ab.py StateLog) do not read them.)
[ -n "$STOP_TICK" -a -z "$ZX_PAD_LOG" ] && export ZX_PAD_LOG=$MODE_WORD
cd $D/cwd
HOME=$D/home ZX_APP_SUPPORT_DIR=$D/app ZX_APP_LOG_DIR=$D/app ZX_SCREENSHOT_DIR=$D/shots ZX_DEBUG_UI=0 \
  ZX_MC_ROOT=$D/mc0 ZX_GS_BACKEND=${ZX_GS_BACKEND:-cpu} $RUNNER $ELF > $LOG 2>&1 &
PID=$!
t0=$(date +%s); why="time limit"
while kill -0 $PID 2>/dev/null; do
  [ $(( $(date +%s) - t0 )) -ge $SECS ] && break
  if [ -n "$STOP_TICK" ]; then
    # vsync ticks only: [state] and [frame:upload] lines ([run:tick] counts something else)
    tick=$(tail -c 16384 $LOG 2>/dev/null | grep -a '^\[state\]\|^\[frame:upload\]' | grep -ao 'tick=[0-9]*' | tail -1 | cut -d= -f2)
    # (and the state file's last line: a runner whose stderr is elsewhere, or quiet, still stops)
    t2=$(tail -n 2 $STATE 2>/dev/null | grep -a '^\[state\]\|^\[pad-state\]' | grep -ao 'tick=[0-9]*' | tail -1 | cut -d= -f2)
    [ -n "$t2" ] && [ ${tick:-0} -lt $t2 ] && tick=$t2
    [ -n "$tick" ] && [ $tick -ge $STOP_TICK ] && { why="tick $tick"; break; }
  fi
  sleep 0.5
done
if kill -0 $PID 2>/dev/null; then
  # (TERM, up to 10 s to exit, then KILL; then wait: the runner gone, its dumps and logs closed)
  kill $PID; for i in {1..40}; do kill -0 $PID 2>/dev/null || break; sleep 0.25; done
  kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
  echo "stopped at $why after $(( $(date +%s) - t0 )) s (unpaced=$ZX_UNPACED): $LOG"
else
  wait $PID; echo "exited rc=$? after $(( $(date +%s) - t0 )) s: $LOG"
fi
