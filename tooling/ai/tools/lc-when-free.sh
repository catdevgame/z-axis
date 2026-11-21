#!/bin/zsh
# Usage: lc-when-free.sh NAME
# Runs `logiccheck.py run NAME` then `compare NAME` once a slot is free:
# up to LC_SLOTS (default 3) runs at once, each with its own card and home
# (logiccheck.py). Logs: /tmp/lc-NAME.log, /tmp/lc-NAME-cmp.log; prints the
# MATCH count; a passing run's dumps are removed. RUNNER, HOME and ZX_*
# pass through. LC_OFF=OFFNAME: compare with --off OFFNAME, a kept run with
# ZX_READABLE=off at the same clock (LC_KEEP_DUMPS=1 keeps its dumps).
#
# A slot is the directory /tmp/ai-lc-slots/<n> holding its waiter's pid; a
# slot whose waiter is gone is freed. Runs started outside the slots
# (logiccheck.py run directly) count too.
N=${LC_SLOTS:-3}
D=/tmp/ai-lc-slots
mkdir -p $D
SLOT=
take() {
  local i p
  for i in {1..$N}; do
    if mkdir $D/$i 2>/dev/null; then echo $$ > $D/$i/pid; SLOT=$i; return 0; fi
    p=$(cat $D/$i/pid 2>/dev/null)
    [ -n "$p" ] && ! kill -0 $p 2>/dev/null && rm -rf $D/$i
  done
  return 1
}
running() { ps -ax -o command | grep -E '^[^ ]*[Pp]ython[0-9.]* [^ ]*logiccheck\.py run ' | grep -c . }
trap '[ -n "$SLOT" ] && rm -rf $D/$SLOT' EXIT INT TERM
# One run of a NAME at a time (tools30): b126's two merge-checks ran this with the same NAME at once, and one's run
# (which removes NAME's old dumps first) or pass (which removes them after) took the dumps the other's compare read
# ("native dump missing" for dumps its run had written). A second one exits 75 at once.
zmodload zsh/system
: >> $D/name-$1.lock   # (zsystem flock opens the file; it does not make it)
if ! zsystem flock -t 0 -f LOCKFD $D/name-$1.lock 2>/dev/null; then
  echo "lc-when-free: another run of $1 is going: exit 75"; exit 75
fi
until take; do sleep 10; done
while [ $(running) -ge $N ]; do sleep 10; done
cd ${0:A:h:h}
# Unpaced by default: same logic, ~35 s instead of ~180 s (ZX_UNPACED=0 for paced).
export ZX_UNPACED=${ZX_UNPACED:-1}
python3 tools/logiccheck.py run "$1" > /tmp/lc-$1.log 2>&1
python3 tools/logiccheck.py compare "$1" ${LC_OFF:+--off} $LC_OFF > /tmp/lc-$1-cmp.log 2>&1
N_MATCH=$(grep -c " MATCH" /tmp/lc-$1-cmp.log)
echo $N_MATCH
# A passing run's RAM dumps (21 x 32 MB) are not needed again: remove them
# (LC_KEEP_DUMPS=1 keeps them); a failing run keeps its dumps for diffing.
[ "$N_MATCH" -ge 21 ] && [ -z "$LC_KEEP_DUMPS" ] && rm -f recomp/dumps/logic/$1-ref[0-9][0-9]-*.ram
