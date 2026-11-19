#!/usr/bin/env python3
"""Quarter pipes, ridden: does a level's ramp kind 1 give AI's Air QPipe?

    qpride.py find GEO INS [N]      quarter pipes with a straight, flat run-up in an AI-format
                                    level (AI's own): kind-1 faces
                                    (TGFT +12) near vertical. Env KIND (1), NYMIN/NYMAX (0, 0.25:
                                    |normal y| of the face), HMIN HMAX RUNUP as args 4-6.
                                    Prints "x z y dx dz top ..." per pipe.
    qpride.py ride NAME TAG x z y dx dz top SPEED [RUNS] [FRAMES] [-v]
                                    on a running runner (nctl.py start NAME): the skater put on
                                    the run-up facing the pipe at SPEED, nothing held (HOLD=n:
                                    forward for n frames), every frame recorded in one batch of
                                    control commands; prints the states, frames of Air QPipe (3)
                                    and Air (2), the height over the take-off, how far past the
                                    lip and where he came down.
Needs dm2_level.py (DM2_LEVEL_PY, default the one beside this file
next to this repo) for the level files' chunks. (dm2level3: Woodward, Movie Lot.)
"""
import math, os, struct, sys, time, collections
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(os.environ.get("DM2_LEVEL_PY", os.path.join(HERE, "dm2_level.py"))))
import dm2_level as D
RAM = "/tmp/qpride-ram.bin"



CTL = os.path.join(HERE, '..', 'recomp', 'run', 'ctl-%s')
def batch(name, commands, timeout=600):
    d = CTL % name
    cmd_path, out_path = os.path.join(d, 'cmd'), os.path.join(d, 'out')
    with open(cmd_path) as f:
        first = sum(1 for _ in f) + 1
    with open(cmd_path, 'a') as f:
        f.write(''.join(c.strip() + '\n' for c in commands))
    ids = {str(first + i): i for i in range(len(commands))}
    last = str(first + len(commands) - 1)
    deadline = time.time() + timeout
    while time.time() < deadline:
        replies = {}
        with open(out_path) as f:
            for line in f:
                i, _, rest = line.partition(' ')
                if i in ids:
                    replies[ids[i]] = rest.rstrip()
        if last in [k for k in ids if ids[k] in replies]:
            return [replies.get(i, '') for i in range(len(commands))]
        time.sleep(0.1)
    raise TimeoutError('batch')


def find_player(d):
    """The player record: the one copy of the position (0x2A76F0) with a name at +0x628."""
    d.do("dump " + RAM)
    b = open(RAM, "rb").read()
    key = b[sweep.POS:sweep.POS + 12]
    i = -1
    while True:
        i = b.find(key, i + 1)
        if i < 0:
            return None
        p = i - 0x90
        if p > 0 and i != sweep.POS and b[p + 0x628:p + 0x62A].isalpha():
            return p


def cmd_find(argv):
    geo, ins = open(argv[0], "rb").read(), open(argv[1], "rb").read()
    N = int(argv[2]) if len(argv) > 2 else 20
    sys.argv = [sys.argv[0], argv[0], argv[1]] + argv[2:]


    NYMAX = float(__import__("os").environ.get("NYMAX", "0.25")); NYMIN = float(__import__("os").environ.get("NYMIN", "0")); KIND = int(__import__("os").environ.get("KIND", "1"))
    HMIN, HMAX, RUNUP = (float(v) for v in (sys.argv[4:7] if len(sys.argv) > 6 else (2, 99, 5)))
    objs = {}
    for obj in D.parse(geo)[0].kids:
        if obj.tag != b'TGEO': continue
        head = next(k for k in obj.kids if k.tag == b'TGHD')
        parts = [k for k in obj.kids if k.tag == b'TGPT']
        if not parts or not any(k.tag == b'TGCI' for k in obj.kids): continue
        best = None
        for part in parts:
            s0 = {k.tag: k for k in part.kids}
            if b'TGVP' not in s0 or b'TGFP' not in s0: continue
            nv0, nf0 = struct.unpack_from('<HH', s0[b'TGPH'].data, 4)
            nv0 = min(nv0, len(s0[b'TGVP'].data) // 12)
            F0 = [struct.unpack_from('<4H', s0[b'TGFP'].data, 8 * i) for i in range(min(nf0, len(s0[b'TGFP'].data) // 8))]
            if F0 and max(max(f[1:]) for f in F0) < nv0 and (best is None or len(F0) > len(best[2])):
                best = (s0, nv0, F0)
        if best is None: continue
        s, nv, F = best
        nf = len(F)
        V = [struct.unpack_from('<3f', s[b'TGVP'].data, 12 * i) for i in range(nv)]
        kinds = [0] * nf
        if b'TGFT' in s:
            desc, count, idx, rs = D.tag_buffer(s[b'TGFT'].data)
            ix = struct.unpack_from('<%dH' % count, idx)
            kinds = [rs[i][12] if i != 0xFFFF else 0 for i in ix]
        objs[struct.unpack_from('<I', head.data, 0x18)[0]] = (V, F, kinds)
    tris = []  # (A, B, C, n, kind, name)
    for c in D.walk(D.parse(ins)):
        if c.tag != b'TGIH': continue
        d = c.data
        if struct.unpack_from('<I', d, 0)[0] & 0x12 != 0x2 or struct.unpack_from('<I', d, 0x44)[0]: continue
        o = objs.get(struct.unpack_from('<I', d, 0x40)[0])
        if not o: continue
        q, p = struct.unpack_from('<4f', d, 8), struct.unpack_from('<3f', d, 0x18)
        V, F, kinds = o
        W = [tuple(a + b for a, b in zip(D.quat_rotate(q, v), p)) for v in V]
        name = d[0x50:].split(b'\0')[0].decode(errors='replace')
        for (fl, a, b, cc), k in zip(F, kinds):
            if not fl & 0x2000: continue
            A, B, C = W[a], W[b], W[cc]
            e1 = [B[i] - A[i] for i in range(3)]; e2 = [C[i] - A[i] for i in range(3)]
            n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
            l = math.sqrt(sum(x * x for x in n))
            if l < 1e-9: continue
            tris.append((A, B, C, [x / l for x in n], k, name, l / 2))
    # The winding: most floor faces' normals should point up (-y).
    up = sum(1 for t in tris if t[3][1] < -0.9); down = sum(1 for t in tris if t[3][1] > 0.9)
    sign = -1.0 if down > up else 1.0
    tris = [(A, B, C, [x * sign for x in n], k, name, area) for A, B, C, n, k, name, area in tris]
    grid = collections.defaultdict(list)
    for t in tris:
        xs = [t[i][0] for i in range(3)]; zs = [t[i][2] for i in range(3)]
        for gx in range(int(math.floor(min(xs) / 4)), int(math.floor(max(xs) / 4)) + 1):
            for gz in range(int(math.floor(min(zs) / 4)), int(math.floor(max(zs) / 4)) + 1):
                grid[(gx, gz)].append(t)
    def ground(x, z, below=-1e9):
        best = None
        for A, B, C, n, k, name, area in grid.get((int(math.floor(x / 4)), int(math.floor(z / 4))), ()):
            if n[1] >= 0: continue
            d = (B[2] - C[2]) * (A[0] - C[0]) + (C[0] - B[0]) * (A[2] - C[2])
            if abs(d) < 1e-12: continue
            l1 = ((B[2] - C[2]) * (x - C[0]) + (C[0] - B[0]) * (z - C[2])) / d
            l2 = ((C[2] - A[2]) * (x - C[0]) + (A[0] - C[0]) * (z - C[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) < -1e-6: continue
            y = l1 * A[1] + l2 * B[1] + l3 * C[1]
            if y < below: continue
            if best is None or y < best[0]: best = (y, n, k, name)
        return best

    if os.environ.get('QPDEBUG'):
        print('tris', len(tris), 'kind1', sum(1 for t in tris if t[4] == 1), 'steep kind1', sum(1 for t in tris if t[4] == 1 and abs(t[3][1]) < NYMAX), 'insts', len(set(t[5] for t in tris)))
    found = []
    for A, B, C, n, k, name, area in tris:
        if k != KIND or abs(n[1]) > NYMAX or abs(n[1]) < NYMIN or area < 0.2: continue
        h = math.hypot(n[0], n[2]); hx, hz = n[0] / h, n[2] / h
        cx = (A[0] + B[0] + C[0]) / 3; cy = (A[1] + B[1] + C[1]) / 3; cz = (A[2] + B[2] + C[2]) / 3
        # Down the pipe's face: the foot is where the ground turns flat, out along the normal.
        prof = []
        for step in range(1, 61):
            t = step * 0.25
            g = ground(cx + hx * t, cz + hz * t, cy - 0.5)
            prof.append((t, g))
        flat = [(t, g) for t, g in prof if g and g[1][1] < -0.98]
        if not flat: continue
        foot_t, foot = flat[0]
        run = [g for t, g in prof if t >= foot_t]
        if len(run) < 24 or any(g is None or g[1][1] > -0.98 or abs(g[0] - foot[0]) > 0.3 for g in run[:24]): continue
        height = foot[0] - cy  # the face's height above the foot (+y down)
        if not HMIN <= height <= HMAX: continue
        start_t = foot_t + RUNUP
        sg = ground(cx + hx * start_t, cz + hz * start_t, cy - 0.5)
        if sg is None:  # no ground at the start
            continue
        found.append((height, cx + hx * start_t, cz + hz * start_t, sg[0], -hx, -hz, cy, foot_t, name))
    found.sort(key=lambda f: -f[0])
    seen = set()
    for h, x, z, y, dx, dz, top, foot_t, name in found:
        key = (name, round(x / 3), round(z / 3))
        if key in seen: continue
        seen.add(key)
        print('%.2f %.2f %.3f %.3f %.3f top %.2f height %.2f foot %.1f %s' % (x, z, y, dx, dz, top, h, foot_t, name))
        N -= 1
        if N == 0: break


def cmd_ride(argv):
    import sweep
    globals()["sweep"] = sweep
    sys.argv = [sys.argv[0]] + argv


    args = [a for a in sys.argv if a != '-v']; verbose = '-v' in sys.argv
    name, tag = args[1], args[2]
    x, z, y, dx, dz, top, speed = (float(v) for v in args[3:10])
    runs = int(args[10]) if len(args) > 10 else 3
    frames = int(args[11]) if len(args) > 11 else 150

    HOLD = int(os.environ.get('HOLD', '0'))  # frames UP is held at the start
    d = sweep.Driver(name); P = find_player(d)
    b = open(RAM, 'rb').read()
    key = b[P + 0x90:P + 0x9C]; copies = []; i = -1
    while True:
        i = b.find(key, i + 1)
        if i < 0: break
        if i != sweep.POS: copies.append(i)
    f = lambda v: hex(struct.unpack('<I', struct.pack('<f', v))[0])
    r0 = d.f32(P + 0xB0, 3); u0 = d.f32(P + 0xC0, 3); f0 = d.f32(P + 0xD0, 3)
    c = [u0[1] * f0[2] - u0[2] * f0[1], u0[2] * f0[0] - u0[0] * f0[2], u0[0] * f0[1] - u0[1] * f0[0]]
    hand = 1.0 if sum(a * b for a, b in zip(c, r0)) >= 0 else -1.0
    up = (0.0, -1.0, 0.0); fw = (dx, 0.0, dz)
    right = tuple(hand * v for v in (up[1] * fw[2] - up[2] * fw[1], up[2] * fw[0] - up[0] * fw[2], up[0] * fw[1] - up[1] * fw[0]))
    def put():
        pk = []
        for c in copies:
            pk += ['poke 0x%x %s' % (c + 4 * i, f(v)) for i, v in enumerate((x, y - 0.05, z))]
        for base in (0xB0, 0x110):
            for row, vec in enumerate((right, up, fw)):
                pk += ['poke 0x%x %s' % (P + base + 16 * row + 4 * i, f(v)) for i, v in enumerate(vec)]
        vel = (dx * speed, 0.0, dz * speed)
        for base in (0xA0, 0x100):
            pk += ['poke 0x%x %s' % (P + base + 4 * i, f(v)) for i, v in enumerate(vel)]
        pk += ['poke 0x%x 0' % (P + 0x80)]
        return pk
    names = {}
    def objname(o):
        if not o: return '-'
        if o not in names:
            w = d.do('peek 0x%x 8' % (o + 0x12C))[0].split()[1:]
            names[o] = b''.join(int(x, 16).to_bytes(4, 'little') for x in w).split(b'\0')[0].decode(errors='replace')
        return names[o]
    for run in range(runs):
        st = d.u32(P + 0x74)
        cmds = ['up UP', 'wait 20'] + put() + ['poke 0x%x 0x%x' % (P + 0x74, (st & ~0xFF) | 2), 'wait 1'] + put() + (['down UP'] if HOLD else [])
        for k in range(frames):
            if HOLD and k == HOLD: cmds += ['up UP']
            cmds += ['peek 0x%x 2' % (P + 0x74), 'peekf 0x%x 3' % (P + 0x90), 'peekf 0x%x 1' % (P + 0x1E8), 'peek 0x%x' % (sweep.FRAME), 'peek 0x%x 3' % (P + 0x194), 'wait 1']
        cmds += ['up UP']
        rep = batch(name, cmds)
        # replies are in command order; pick ours by position.
        start = len(cmds) - 1 - 6 * frames - (1 if HOLD and HOLD < frames else 0)
        rows = []
        for k in range(frames):
            o = start + 6 * k + (1 if HOLD and k >= HOLD else 0)
            a = rep[o:o + 5]
            w = a[0].split()[1:]; st_, fl = int(w[0], 16) & 0xFF, int(w[3], 16) if len(w) > 3 else 0
            flags = int(a[0].split()[2], 16) if len(a[0].split()) > 2 else 0
            p = [float(v) for v in a[1].split()[1:]]; sp = float(a[2].split()[1]); fr = int(a[3].split()[1], 16)
            ws = a[4].split()[1:]
            rows.append((st_, p, sp, fr, int(ws[0], 16), int(ws[2], 16)))
        if verbose:
            last = None
            for r in rows:
                if r[0] != last or r[3] % 6 == 0:
                    print('   fr %d st %d pos %.2f %.2f %.2f sp %.1f surface %d object %s' % (r[3] - rows[0][3], r[0], r[1][0], r[1][1], r[1][2], r[2], r[4] if r[4] < 0x10000 else -1, objname(r[5])))
                last = r[0]
        states = [r[0] for r in rows]
        air = [i for i, r in enumerate(rows) if r[0] in (2, 3, 4) and i > 5]
        first_air = air[0] if air else None
        apex = min((rows[i][1][1] for i in air), default=None)
        landed = None
        if first_air is not None:
            landed = next((rows[i] for i in range(first_air, len(rows)) if rows[i][0] not in (2, 3, 4)), None)
        take = rows[first_air][1] if first_air is not None else None
        # Out over the lip: the most the skater got past the take-off along the approach.
        past = max((dx * (rows[i][1][0] - take[0]) + dz * (rows[i][1][2] - take[2]) for i in air), default=None) if take else None
        seq = []
        for s in states:
            if not seq or seq[-1][0] != s: seq.append([s, 0])
            seq[-1][1] += 1
        res = dict(first_air_state=rows[first_air][0] if first_air is not None else None,
                   qp_air=states.count(3), air=states.count(2), spine=states.count(4),
                   above_takeoff=(take[1] - apex) if take else None, past_lip=past,
                   landed_state=landed[0] if landed else None,
                   drift=math.hypot(landed[1][0] - take[0], landed[1][2] - take[2]) if landed and take else None,
                   speed_up=rows[first_air - 1][2] if first_air else None,
                   takeoff_from='%s/%d' % (objname(rows[first_air - 1][5]), rows[first_air - 1][4]) if first_air else None)
        print('RUN %s %d %s' % (tag, run, ' '.join('%s=%s' % (k, ('%.2f' % v) if isinstance(v, float) else v) for k, v in res.items())), flush=True)
        print('   states', ' '.join('%d:%d' % tuple(s) for s in seq[:24]), flush=True)


if __name__ == "__main__":
    {"find": cmd_find, "ride": cmd_ride}[sys.argv[1]](sys.argv[2:])
