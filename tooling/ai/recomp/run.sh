#!/bin/zsh
# Usage: run.sh <seconds> <logname> [extra env...]
# Runs the runner for N seconds, logs to logs/<logname>.log, screenshots window at the end.
# The script's own folder (not ~/...), so runs with a scratch HOME (which
# keeps the runner away from the user's settings, saves and logs) still
# find the repo.
R=${0:A:h}
SECS=${1:-20}; NAME=${2:-run}
source ${R:h:h}/paths.sh   # the shared environment (no sound: ZAXIS_AUDIO)
cd $R/run
# The disc is read from recomp/disc-root (the ELF's directory); set PS2X_CD_IMAGE to use an ISO instead.
export PS2X_DEBUG_UI=${PS2X_DEBUG_UI:-0}
export PS2X_MC_ROOT=${PS2X_MC_ROOT:-$R/run/mc0}
# The CPU renderer unless PS2X_GS_BACKEND=gpu is given.
export PS2X_GS_BACKEND=${PS2X_GS_BACKEND:-cpu}
${RUNNER:-${R:h:h:h}/out/build-ai/ps2xRuntime/ps2EntryRunner} $R/disc-root/SLUS_203.27 > $R/logs/$NAME.log 2>&1 &
PID=$!
# SHOT_EVERY=<n> also captures logs/<name>-<t>.png every n seconds.
t=0
# RUN_UNTIL=<file>: stop once that file is written and its size holds
# (logiccheck: the last checkpoint dump).
STOPPED=
while [ $t -lt $SECS ] && kill -0 $PID 2>/dev/null; do
  sleep 1; t=$((t+1))
  if [ -n "$RUN_UNTIL" ] && [ -f "$RUN_UNTIL" ]; then
    s1=$(stat -f %z "$RUN_UNTIL"); sleep 1; t=$((t+1))
    if [ "$s1" = "$(stat -f %z "$RUN_UNTIL")" ]; then STOPPED=1; break; fi
  fi
  if [ -n "$SHOT_EVERY" ] && [ $((t % SHOT_EVERY)) -eq 0 ]; then
    W=$($R/tools/winid $PID); [ -n "$W" ] && screencapture -x -o -l $W $R/logs/$NAME-$t.png
  fi
done
if kill -0 $PID 2>/dev/null; then
  WID=$($R/tools/winid $PID)
  [ -n "$WID" ] && screencapture -x -o -l $WID $R/logs/$NAME.png
  # (TERM, up to 10 s to exit, then KILL; then wait: the runner gone and its dumps closed before a compare)
  kill $PID; for i in {1..40}; do kill -0 $PID 2>/dev/null || break; sleep 0.25; done
  kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
  if [ -n "$STOPPED" ]; then echo "stopped after $t s ($RUN_UNTIL written)"; else echo "killed after $SECS s"; fi
else
  wait $PID; echo "exited rc=$?"
fi
wc -l $R/logs/$NAME.log
