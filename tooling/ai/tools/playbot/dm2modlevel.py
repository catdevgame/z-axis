#!/usr/bin/env python3
"""Dave Mirra BMX 2: a custom level served from a Mods folder in the park slot (dm2_level_convert.py's output: the
level as a park theme), its park card made in DM2's own editor, ridden from FREERIDE's level select by bikepark.py's
rider.

    dm2modlevel.py build FOLDER [THEME]          the level's navigation map from the folder's own files
                                                 (FOLDER/ASSETS/EDITOR/THEME/THEME.*; THEME LEWAREHO): DM2's
                                                 collision faces and grind edges (dm2_level.py, dm2_xbox.py) as a
                                                 park's geometry, parkmap.py's map and features; kept as
                                                 $PLAYDM2/level/THEME.parkmap
    dm2modlevel.py card NAME FOLDER INDEX COLUMN ROW TURNS
                                                 the park for it: PARK EDITOR, NEW PARK, theme INDEX (the mod's
                                                 THEMES.TXT's added theme), the start at (COLUMN, ROW) with TURNS
                                                 quarter turns (the converter's report gives all four), SAVE PARK to
                                                 the card's first slot; the card is $PLAYDM2/runs/NAME/mc0
    dm2modlevel.py ride NAME FOLDER CARD TICKS [SEED] [THEME]
                                                 FREERIDE, the first rider and bike, level select's 8 (the park), the
                                                 card's first park: the level, then TICKS pad reads of riding a tour of
                                                 the map's features (bikepark.py's play: dm2.Meter's numbers, the
                                                 features reached)

Every run has ZX_MODS_DIR=FOLDER and a tick limit; everything else is dm2.py's (its clock, card, scratch, runner).
SHOTS=1: pictures.
"""
import collections
import os
import shutil
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import core  # noqa: E402
import dm2  # noqa: E402
import parkmap  # noqa: E402


def level_files(folder, theme='LEWAREHO'):
    base = os.path.join(folder, 'ASSETS', 'EDITOR', theme.upper(), theme.upper())
    out = {}
    for ext in ('GEO', 'INS', 'DAT'):
        with open(base + '.' + ext, 'rb') as f:
            out[ext] = f.read()
    return out


def folder_map(folder, theme='LEWAREHO'):
    """The level's collidable faces (the faces DM2's trees list, by instance) and grindable edges, as a park's
    geometry, then parkmap.build's map; the DAT's points kept."""
    import dm2_level as dl
    import dm2_xbox
    files = level_files(folder, theme)
    geo = parkmap.Geometry()
    for g in dl.collision_triangles(files['GEO'], files['INS']):
        piece = len(geo.pieces)
        geo.pieces.append(dict(name='piece%d' % piece, model='piece%d' % piece, at=((g[0] + g[1]) / 2, 0.0, (g[2] + g[3]) / 2)))
        for t in g[4]:
            geo.faces.append((t, False, piece))
    geo.edges = [(a, b, name, kind) for a, b, name, kind in dm2_xbox.grind_edges(files['GEO'], files['INS'])]
    m = parkmap.build(geo, theme.upper(), extra=dict(game='dm2'))
    points = []
    for c in dl.parse(files['DAT'])[0].kids:
        if c.tag == b'SDPT':
            kind, nm = struct.unpack_from('<II', c.data, 4)
            points.append(dict(kind=kind, name=nm, pos=struct.unpack_from('<3f', c.data, 0x10)))
    m['points'] = points
    return m


def build(folder, theme='LEWAREHO'):
    m = folder_map(folder, theme)
    path = os.path.join(dm2.SCRATCH, 'level', theme.upper() + '.parkmap')
    os.makedirs(os.path.dirname(path), exist_ok=True)
    parkmap.save(m, path)
    print('%s from %s: %d cells, %d rails (%s), %d walls; features %s; kept as %s' % (
        theme, folder, len(m['nodes']), len(m['rails']), ', '.join('%.0f m' % r['length'] for r in m['rails']),
        len(m['walls']), dict(collections.Counter(f['kind'] for f in m['features'])), path), flush=True)
    return m


def card(name, folder, index, column, row, turns):
    """An empty park on the mod's theme: only its start, saved to the card's first slot."""
    import dm2park
    out = {}

    def program(bot):
        ok = yield from dm2park.enter(bot, index)
        if not ok:
            return
        out['theme'] = bot.ed.u8(dm2park.PE + dm2park.THEME)
        out['size'] = bot.ed.size()
        yield from dm2park.start(bot, row, column, turns)
        out['start'] = bot.ed.start()
        yield from dm2park.save_park(bot)
        out['saved'] = True

    bot, run = dm2park.session(name, program, ticks=40000, env={'ZX_MODS_DIR': os.path.abspath(folder)})
    cardir = os.path.join(dm2.SCRATCH, 'runs', name, 'mc0')
    print('card %s: %s; %s' % (cardir, out, 'BMX2PARK.DAT there' if os.path.exists(os.path.join(cardir, parkmap.DM2_PARK_FILE)) else
                                'NO BMX2PARK.DAT'), flush=True)
    return out


def ride(name, folder, cardir, ticks, seed=1, theme='LEWAREHO'):
    """bikepark.py's DM2 ride (FREERIDE, level select's 8, the card's first park) with the mod and the level's own
    map."""
    import bikepark
    import dm2routes
    s = dm2routes.Script()
    plain = s.presses
    s.presses = lambda *b, after=dm2.MENU_GAP: plain(*b, after=after)
    s.presses(*['DOWN'] * dm2routes.MAIN['freeride'])
    for _ in range(4):                 # FREERIDE, the rider, the bike, the modifiers
        s.press('CROSS', after=240)
    s.presses(*['RIGHT'] * 8)          # the park
    run_dir = os.path.join(dm2.SCRATCH, 'runs', name)
    shutil.rmtree(run_dir, ignore_errors=True)
    shutil.copytree(cardir, os.path.join(run_dir, 'mc0'))
    cfg = dm2.run_config(name, s.text(), env={'ZX_HEADLESS': '1', 'ZX_MODS_DIR': os.path.abspath(folder)},
                         max_secs=max(900, ticks // 10), gs=bikepark.SHOTS)
    cfg.menu_end = s.t + 30
    park = bikepark.Park(world=lambda r, c: (0.0, 0.0, 0.0), steps=[], zones=())

    def enter(r):
        yield from bikepark.tap('CROSS', gap=300)
        for k in range(12):
            if r.st.riding:
                break
            yield from bikepark.tap('CROSS', gap=400)
        for _ in range(3000):
            if r.st.riding and r.st.frame > 120:
                break
            yield core.NOTHING
        if bikepark.SHOTS:
            print('shot', r.run.shot('riding'), flush=True)
        r.say('riding:', r.st.riding, 'session %#x level %d' % (r.st.session, r.st.level))
        return r.st.riding

    bikepark.play(name, park, dm2.read, cfg, enter, ticks, seed, os.path.join(dm2.SCRATCH, 'measure'),
                  make_map=lambda r: folder_map(folder, theme))


def main(argv):
    if not argv:
        raise SystemExit(__doc__)
    cmd, args = argv[0], argv[1:]
    if cmd == 'build':
        build(*args[:2])
    elif cmd == 'card':
        card(args[0], args[1], int(args[2]), int(args[3]), int(args[4]), int(args[5]))
    elif cmd == 'ride':
        ride(args[0], args[1], args[2], int(args[3]), int(args[4]) if len(args) > 4 else 1, args[5] if len(args) > 5 else 'LEWAREHO')
    else:
        raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
