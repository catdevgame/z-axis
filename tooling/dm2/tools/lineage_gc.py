#!/usr/bin/env python3
"""lineage_gc.py: date the engine's ZGE2 -> ZGE3 -> ZGE4 changes with the GameCube builds
(MATCH3.md section 6, "The engine's lineage").

    python -I tools/lineage_gc.py OUT.tsv [--notes RESULTS.jsonl --run NAME]

For each row of the work database's engine_lineage whose PS2 bodies differ at a step (ZGE2 to ZGE3:
DM2 and AI; ZGE3 to ZGE4: AI and BMX XXX), the two games' GameCube counterparts (high or medium pairs)
are compared as PowerPC words masked for addresses (calls out of the function, lis and the @l halves
built on it, r2/r13 small-data offsets). The same masked code on GameCube says the later engine's
form was already in the earlier game's GameCube build. The control is the same comparison where the
PS2 bodies are identical: it says whether the two GameCube builds share a compiler (AI's and BMX
XXX's do, about 80 % equal; DM2's and AI's do not, about 15 %), so only an equal pair is evidence.
OUT.tsv lists the dated changes; --notes writes each as a `history` note (kind note) on AI's
function. Inputs are disc derived: outputs go to scratch. (`python` = the venv with duckdb.)
"""
import collections, datetime, json, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import workdb  # noqa: E402  (re-runs itself with the venv's Python when duckdb is missing)
import duckdb  # noqa: E402

GC = ('ai_gc', 'dm2_gc', 'bmx_gc')
WHEN = {'dm2_ps2': 'DM2\'s PS2 build (August 2001)', 'ai_ps2': 'AI\'s PS2 build (April 2002)',
        'dm2_gc': 'DM2\'s GameCube release (built after September 2001, its Dolphin OS kernel\'s date)',
        'ai_gc': 'AI\'s GameCube release (built after June 2002, its Dolphin OS kernel\'s date)'}


def dol_words(d, secs, a, e):
    for s, sz, o in secs:
        if s <= a < s + sz:
            b = d[o + a - s:o + min(sz, e - s)]
            return struct.unpack('>%dI' % (len(b) // 4), b[:len(b) // 4 * 4])
    return ()


def masked(ws, a, e):
    out, lis = [], set()
    for i, w in enumerate(ws):
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        if op == 18:  # b, bl: a target outside the function is an address
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            t = li if w & 2 else a + 4 * i + li
            if not a <= t < e:
                w &= 0xFC000003
        elif op == 15 and ra == 0:  # lis
            lis.add(rd)
            w &= 0xFFFF0000
        elif op in (14, 15, 24) or 32 <= op <= 55:  # addi, addis, ori, D-form loads and stores
            base = rd if op == 24 else ra
            if base in (2, 13) or base in lis:
                w &= 0xFFFF0000
        elif op in (56, 57, 60, 61):  # psq_l, psq_st: 12-bit offsets
            if ra in (2, 13) or ra in lis:
                w &= 0xFFFFF000
        out.append(w)
    return tuple(out)


def gc_code():
    code = {}
    for b in GC:
        d = open(workdb.src(b + '.exe'), 'rb').read()
        offs = struct.unpack_from('>18I', d, 0)
        addrs = struct.unpack_from('>18I', d, 0x48)
        sizes = struct.unpack_from('>18I', d, 0x90)
        secs = [(addrs[i], sizes[i], offs[i]) for i in range(18) if sizes[i]]
        for ln in open(workdb.src(b + '.jsonl')):
            r = json.loads(ln)
            if 'a' in r:
                code[(b, r['a'])] = masked(dol_words(d, secs, r['a'], r['e']), r['a'], r['e'])
    return code


def h(v):
    return '' if v is None else '0x%08X' % v


def main(argv):
    if not argv:
        sys.exit(__doc__)
    out = argv[0]
    notes = argv[argv.index('--notes') + 1] if '--notes' in argv else None
    run = argv[argv.index('--run') + 1] if '--run' in argv else 'lineage_gc'
    con = duckdb.connect(workdb.DB, read_only=True)
    gc = {(b, a): x for a, b, x in con.execute(
        "SELECT a_addr, b_build, b_addr FROM matches WHERE b_build IN ('ai_gc', 'dm2_gc', 'bmx_gc') "
        "AND confidence IN ('high', 'medium')").fetchall()}
    code = gc_code()
    rows = con.execute("SELECT zge2, zge3, zge4, zge2_zge3, zge3_zge4, name, file FROM engine_lineage "
                       "WHERE zge3 IS NOT NULL ORDER BY zge3").fetchall()
    ctl = collections.Counter()
    found = []
    for z2, z3, z4, c23, c34, name, f in rows:
        g2 = gc.get(('dm2_gc', z2)) if z2 is not None else None
        g3 = gc.get(('ai_gc', z3))
        g4 = gc.get(('bmx_gc', z4)) if z4 is not None else None
        k2 = code.get(('dm2_gc', g2)) if g2 else None
        k3 = code.get(('ai_gc', g3)) if g3 else None
        k4 = code.get(('bmx_gc', g4)) if g4 else None
        for step, cls, ka, kb in (('ZGE2-ZGE3', c23, k2, k3), ('ZGE3-ZGE4', c34, k3, k4)):
            if ka is None or kb is None or not cls:
                continue
            same = ka == kb
            ctl[(step, 'PS2 identical' if cls == 'identical' else 'PS2 differs', same)] += 1
            if same and cls != 'identical':
                found.append((step, z2, z3, z4, g2, g3, g4, cls, name, f))
    for k in sorted(ctl):
        print('%s, %s: GameCube %s %d' % (k[0], k[1], 'equal' if k[2] else 'differs', ctl[k]))
    with open(out, 'w') as fo:
        fo.write('# lineage_gc.py: engine changes the GameCube builds date. Columns: step, ZGE2 (DM2), ZGE3 (AI), '
                 'ZGE4 (BMX XXX), DM2 GC, AI GC, BMX GC, PS2 class, name, AI file.\n')
        for r in found:
            fo.write('\t'.join([r[0]] + [h(v) for v in r[1:7]] + [r[7], r[8] or '', r[9] or '']) + '\n')
    print(len(found), 'dated changes ->', out)
    if not notes:
        return
    try:
        c = subprocess.run(['git', '-C', HERE, 'log', '-1', '--format=%h'], capture_output=True,
                           text=True).stdout.strip()
        if c and subprocess.run(['git', '-C', HERE, 'status', '--porcelain', '--', 'lineage_gc.py', 'workdb.py'],
                                capture_output=True, text=True).stdout.strip():
            c += '+changes'
    except OSError:
        c = ''
    ts = datetime.datetime.now().replace(microsecond=0).isoformat()
    with open(notes, 'w') as fo:
        for i, (step, z2, z3, z4, g2, g3, g4, cls, name, f) in enumerate(found, 2):
            nm = name or 'fn_%06X' % z3
            if step == 'ZGE3-ZGE4':
                t = ("%s: ZGE4's PS2 body (BMX XXX %s, %s to AI's) differs from ZGE3's, but AI's GameCube build (%s) "
                     "has the same code as BMX XXX's GameCube build (%s), so ZGE4's form was already in the engine of "
                     "%s: the change came after %s, or the PS2 difference is PS2-only code."
                     % (nm, h(z4), cls, h(g3), h(g4), WHEN['ai_gc'], WHEN['ai_ps2']))
            else:
                t = ("%s: ZGE2's PS2 body (DM2 %s, %s to AI's) differs from ZGE3's, but DM2's GameCube build (%s) "
                     "has the same code as AI's GameCube build (%s), so ZGE3's form was already in the engine of "
                     "%s: the change came after %s, or the PS2 difference is PS2-only code."
                     % (nm, h(z2), cls, h(g2), h(g3), WHEN['dm2_gc'], WHEN['dm2_ps2']))
            fo.write(json.dumps(dict(kind='note', build='ai_ps2', addr=h(z3), topic='history', text=t, run=run,
                                     generator='lineage_gc.py ' + (c or 'uncommitted'), agent=run, ts=ts,
                                     source='%s:%d (lineage_gc.py)' % (out, i))) + '\n')
    print(len(found), 'notes ->', notes)


if __name__ == '__main__':
    main(sys.argv[1:])
