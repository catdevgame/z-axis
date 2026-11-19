#!/usr/bin/env python3
"""dead_code_dm2.py [OUT.tsv]: Dave Mirra BMX 2's dead-code table (PS2Recomp docs/DEAD-CODE.md,
"DM2"), AI's tools/dead_code.py's groups for a game with no sweep manifest: every function of DM2's
PS2 ELF that nothing reaches, with the evidence for each.

A function is a candidate when the ELF has no call or jump to it from another function (the work
database's calls), no code reference to its address (func_globals: a lui/addiu pair or a word a
function loads), no data word equal to its address (the ELF's loaded data sections: tables of
functions, vtables, state tables), and no recording of it in DM2's corpus (the corpus index, one
line per recording file: CORPUS_INDEX). Kinds: `dead` (no reference at all) and `dead-callers` (its
only callers or references are themselves candidates, to a fixed point: AI's "called only from such
code"). Like AI's `dead` flag, it means no reference was found, not that the function is proven
unreachable: computed calls through a pointer the code builds are not seen. The roots are the
ELF's entry, the recorded functions and the data words.

Groups: library (layer sdk: Sony's libraries, libc, libm, libgcc), engine (below DM2's engine end
0x163280, zgui.c the last module: ZGE2), game (above it). Columns: address, group, kind, bytes,
name (the work database's best name, else Ghidra's), callers (all, then those not candidates),
code references, data words, AI PS2 counterpart (address, class, AI's dead-code flag), BMX XXX PS2
counterpart (class), the other releases with a high or medium counterpart, module (engine: AI's
readable file; game: the nearest preceding source path), strings. Holds strings from the game's
ELF, so it stays in scratch. Run with the venv that has duckdb after `workdb.py build`:

    $ZAXIS_SCRATCH/venv/bin/python -I tools/dead_code_dm2.py [OUT.tsv]
"""
import bisect, collections, os, struct, sys

import duckdb

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import zpaths  # noqa: E402  (tooling/paths.env)
HOME = os.path.expanduser('~')
SCRATCH = os.environ.get('AI_SCRATCH', zpaths.ZAXIS_SCRATCH)
DB = os.environ.get('WORKDB', SCRATCH + '/workdb/work.duckdb')
ELF = os.environ.get('DM2_ELF', SCRATCH + '/match3/elf/SLUS_201.59')
# One line per recording file of DM2's corpus, folder TAB file (src2plan's index is gone): on the Linux worker,
# cd $LINUX_WORKER_ROOT/reccorpus/dm2-v1 && find . -name '*.fnrec' -printf '%h\t%f\n'
CORPUS_INDEX = os.environ.get('CORPUS_INDEX')
AI_DEAD = os.environ.get('AI_DEAD', SCRATCH + '/dead-code/dead-code.tsv')
OUT = sys.argv[1] if len(sys.argv) > 1 else SCRATCH + '/dead-code/dm2-dead-code.tsv'
ENGINE_END = 0x163280
RELEASES = ('dm2_xbox', 'dm2_gc')


def data_words(path):
    """Every aligned word of the ELF's loaded, non-executable sections."""
    b = open(path, 'rb').read()
    shoff, = struct.unpack_from('<I', b, 0x20)
    shentsize, shnum = struct.unpack_from('<HH', b, 0x2E)
    words = collections.Counter()
    for i in range(shnum):
        name, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', b, shoff + i * shentsize)
        if typ != 1 or not flags & 2 or flags & 4:
            continue
        for j in range(0, size - 3, 4):
            words[struct.unpack_from('<I', b, off + j)[0]] += 1
    return words


def main():
    db = duckdb.connect(DB, read_only=True)
    q = lambda s, a=(): db.execute(s, list(a)).fetchall()
    fs = q("SELECT f.addr, f.bytes, f.layer, coalesce(bn.name, f.name) FROM functions f LEFT JOIN best_names bn "
           "ON bn.build = f.build AND bn.addr = f.addr WHERE f.build = 'dm2_ps2' ORDER BY f.addr")
    starts = {a for a, *_ in fs}
    callers = collections.defaultdict(set)
    for c, t in q("SELECT DISTINCT caller, callee FROM calls WHERE build = 'dm2_ps2'"):
        if c != t:
            callers[t].add(c)
    for f, g in q("SELECT DISTINCT func, addr FROM func_globals WHERE build = 'dm2_ps2'"):
        if g in starts and f != g:
            callers[g].add(f)
    coderef = collections.Counter(g for f, g in q("SELECT DISTINCT func, addr FROM func_globals "
                                                  "WHERE build = 'dm2_ps2'") if g in starts)
    words = data_words(ELF)
    recorded = set()
    if not CORPUS_INDEX:
        sys.exit('CORPUS_INDEX: the DM2 corpus index (folder TAB file per recording; see the top of this file)')
    for ln in open(CORPUS_INDEX):
        d, f = ln.split('\t')[:2]
        if f.startswith('dm2-') and f.endswith('.fnrec'):
            recorded.add(int(f[4:12], 16))
    entry, = struct.unpack_from('<I', open(ELF, 'rb').read(0x40), 0x18)
    # candidates: no data word, no recording, not the ELF's entry; then no caller outside the
    # candidates, to a fixed point
    cand = {a for a, *_ in fs if not words[a] and a not in recorded and a != entry}
    while True:
        drop = {a for a in cand if callers[a] - cand}
        if not drop:
            break
        cand -= drop
    ai = {}
    for a, x, cls in q("SELECT a_addr, b_addr, cls FROM matches WHERE a_build = 'ai_ps2' AND b_build = 'dm2_ps2'"):
        ai[x] = (a, cls)
    ai_flag = {}
    if os.path.exists(AI_DEAD):
        for ln in open(AI_DEAD):
            c = ln.rstrip('\n').split('\t')
            if not ln.startswith('#') and len(c) > 2:
                ai_flag[int(c[0], 16)] = c[2]
    ai_file = dict(q("SELECT addr, file FROM readable"))
    bmx = {a: (x, cls) for a, x, cls in q("SELECT a_addr, b_addr, cls FROM matches WHERE a_build = 'dm2_ps2' "
                                          "AND b_build = 'bmx_ps2'")}
    rel = collections.defaultdict(set)
    for a, b in q("SELECT a_addr, b_build FROM matches WHERE a_build = 'dm2_ps2' AND confidence IN ('high', 'medium') "
                  "AND b_build IN ('dm2_xbox', 'dm2_gc')"):
        rel[a].add(b)
    anchors = q("SELECT func, path FROM file_anchors WHERE build = 'dm2_ps2' ORDER BY func")
    astarts = [a for a, _ in anchors]
    strings = collections.defaultdict(list)
    for f, t in q("SELECT func, text FROM func_strings WHERE build = 'dm2_ps2' ORDER BY func, addr"):
        if len(strings[f]) < 3:
            strings[f].append(t.replace('\t', ' ').replace('\n', ' ').replace('\r', ' ')[:60])

    def module(a, group):
        if a in ai and ai_file.get(ai[a][0]):
            return ai_file[ai[a][0]]
        i = bisect.bisect_right(astarts, a) - 1
        if group == 'game' and i >= 0:
            return '~' + anchors[i][1].replace('./src/com/', '')
        return '-'

    count = collections.Counter()
    nbytes = collections.Counter()
    os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
    with open(OUT, 'w') as f:
        f.write('# tools/dead_code_dm2.py: DM2 PS2 functions nothing reaches (docs/DEAD-CODE.md, "DM2"). Columns:\n'
                '# address, group, kind, bytes, name, callers (all/live), code refs, data words, AI counterpart '
                '(address class flag), BMX XXX class, DM2 Xbox/GC, module, strings.\n')
        for a, size, layer, name in fs:
            if a not in cand:
                continue
            group = 'library' if layer == 'sdk' else ('engine' if a < ENGINE_END else 'game')
            kind = 'dead' if not callers[a] else 'dead-callers'
            count[(group, kind)] += 1
            nbytes[group] += size
            ap, acls = ai.get(a, (None, None))
            aic = '0x%06X %s %s' % (ap, acls, ai_flag.get(ap, 'live')) if ap is not None else '-'
            f.write('\t'.join([
                '0x%06X' % a, group, kind, str(size), name or '-',
                '%d/%d' % (len(callers[a]), len(callers[a] - cand)), str(coderef[a]), str(words[a]), aic,
                bmx[a][1] if a in bmx else '-', ','.join(sorted(rel[a])) or '-', module(a, group),
                ' | '.join(strings[a]) or '-']) + '\n')
    for k in sorted(count):
        print('%-8s %-13s %5d' % (k + (count[k],)))
    print('bytes:', dict(nbytes))
    print('wrote', OUT, sum(count.values()), 'rows')


if __name__ == '__main__':
    main()
