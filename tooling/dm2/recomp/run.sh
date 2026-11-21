#!/bin/zsh
# Usage: run.sh <seconds> <logname>
# Runs the DM2 dev runner for N seconds with a scratch HOME (never the user's
# folders), logs to logs/<logname>.log and captures the window at the end.
# SHOT_EVERY=<n>: also logs/<name>-<t>.png every n seconds.
# RUNNER=<path> picks another runner build.
R=${0:A:h}
SECS=${1:-20}; NAME=${2:-run}
SCRATCH=${DM2_SCRATCH:-/tmp/ai-dm2}
source ${R:h:h}/paths.sh   # the shared environment (no sound: ZX_AUDIO)
mkdir -p $SCRATCH/home $SCRATCH/mc0 $SCRATCH/mc1 $R/logs $R/run
cd $R/run
export HOME=$SCRATCH/home
export ZX_APP_SUPPORT_DIR=$SCRATCH/home/support
export ZX_APP_LOG_DIR=$SCRATCH/home/logs
export ZX_DEBUG_UI=${ZX_DEBUG_UI:-0}
export ZX_MC_ROOT=${ZX_MC_ROOT:-$SCRATCH/mc0}
export ZX_GS_BACKEND=${ZX_GS_BACKEND:-cpu}
# (from R, made before the cd: ${0:A} after it resolves a relative $0 against run/)
[ -x $SCRATCH/winid ] || cp ${R:h:h}/ai/recomp/tools/winid $SCRATCH/winid 2>/dev/null
RUNNER=${RUNNER:-${R:h:h:h}/out/build-coll/runtime/zx-runner}
$RUNNER $R/disc-root/SLUS_201.59 > $R/logs/$NAME.log 2>&1 &
PID=$!
t=0
while [ $t -lt $SECS ] && kill -0 $PID 2>/dev/null; do
  sleep 1; t=$((t+1))
  if [ -n "$SHOT_EVERY" ] && [ $((t % SHOT_EVERY)) -eq 0 ]; then
    W=$($SCRATCH/winid $PID); [ -n "$W" ] && screencapture -x -o -l $W $R/logs/$NAME-$t.png
  fi
done
if kill -0 $PID 2>/dev/null; then
  W=$($SCRATCH/winid $PID)
  [ -n "$W" ] && screencapture -x -o -l $W $R/logs/$NAME.png
  kill $PID; sleep 1; kill -9 $PID 2>/dev/null
  echo "killed after $SECS s"
else
  wait $PID; echo "exited rc=$?"
fi
wc -l $R/logs/$NAME.log
