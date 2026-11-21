#!/bin/zsh
# make-generated.sh RECOMP CONFIG FOLDER KEY: the generated code of CONFIG made by RECOMP (this tree's ps2_recomp) into
# FOLDER-KEY (KEY: generated_key.py over every game's config, the set's key), unless it is there already; prints the
# folder. Made aside (FOLDER-<key>.making) and
# renamed: a folder of that name is always whole. The config's output path is replaced by the folder's.
set -e
RECOMP=$1; CONFIG=$2; BASE=$3; KEY=$4
[[ -x $RECOMP && -f $CONFIG && -n $BASE && $KEY =~ '^[0-9a-f]+$' ]] || { echo "usage: make-generated.sh RECOMP CONFIG FOLDER KEY" >&2; exit 2; }
OUT=$BASE-$KEY
if [[ -f $OUT/register_functions.cpp ]]; then echo $OUT; exit 0; fi
TMP=$OUT.making
rm -rf $TMP; mkdir -p ${TMP:h}
CFG=$TMP.toml
# (the config's relative paths as its own folder resolves them; the output the folder being made)
python3 -I - $CONFIG $TMP $CFG <<'PY'
import os, re, sys
src, out, dst = sys.argv[1:4]
base = os.path.dirname(os.path.abspath(src))
text = open(src, encoding='utf-8').read()
def absolute(m):
    p = m.group(2)
    return '%s = "%s"' % (m.group(1), p if os.path.isabs(p) else os.path.normpath(os.path.join(base, p)))
text = re.sub(r'^(input|ghidra_output)\s*=\s*"([^"]*)"', absolute, text, flags=re.M)
text = re.sub(r'^output\s*=\s*"[^"]*"', 'output = "%s"' % out, text, flags=re.M)
open(dst, 'w', encoding='utf-8').write(text)
PY
$RECOMP $CFG > $TMP.log 2>&1 || { echo "make-generated.sh: $RECOMP failed (log $TMP.log)" >&2; exit 1; }
rm -f $CFG
mv $TMP.log $TMP/recompile.log
mv $TMP $OUT
echo $OUT
