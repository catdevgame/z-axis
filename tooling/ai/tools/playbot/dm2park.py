#!/usr/bin/env python3
"""Dave Mirra BMX 2: the park players' test park built in DM2's own park
editor, saved to the card the game's way, and ridden.

    dm2park.py probe NAME                 orientation pictures: pieces at each turn, the camera tilted down
    dm2park.py build NAME                 the six-zone park on Warehouse (24 x 24, its 20 x 20 floor), saved: the
                                          card's BASLUS-20159BMX2PARK/BMX2PARK.DAT (DM2 has no native park files)
    dm2park.py ride NAME CARD TICKS [SEED] the saved park (CARD: a card folder holding it) ridden from FREERIDE's
                                          level 8, or with RIDE=test the editor's Test Run; dm2.Meter's numbers

The editor is closed on its own record (g_parkEditor: the cursor's cell, the tool, each cell's level and object), the
held object (g_heldObject, g_heldRotation), the tool roll's entry (g_currentToolEntry), the rail tool's slot and type
(g_railSlot, g_railType, g_railCount) and the park's capacity (g_parkCapacityUsed of g_parkCapacity). SHOTS=1:
pictures. Scratch: $PLAYDM2.
"""
import json
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import dm2  # noqa: E402
import zpaths  # noqa: E402
from core import pad, NOTHING  # noqa: E402

SHOTS = os.environ.get('SHOTS') == '1'
SYMBOLS = zpaths.tree_path(zpaths.ZAXIS_MAIN, 'runtime', 'tools', 'readable_symbols.tsv')


def symbols(game):
    table = {}
    with open(SYMBOLS) as f:
        for line in f:
            w = line.rstrip('\n').split('\t')
            if len(w) >= 5 and w[0] == 'data' and w[4] == game:
                table[w[1]] = int(w[2], 16)
    return table


SYM = symbols('dm2')
PE = SYM['g_parkEditor']
# ParkEditor (game/dm2/editor/park_editor.h): the cursor (s8 column, row), the theme and size, the cells (0x14 bytes:
# s8 level, the PlacedObject: +8 catalog index, +0xA quarter turns), the start, the tool
CURSOR_COLUMN, CURSOR_ROW, THEME, ROWS, COLUMNS, CELLS, CELL_BYTES = 0x04, 0x05, 0x91, 0x92, 0x93, 0x94, 0x14
START_OFFSET, TOOL = 0x14094, 0x142B4
TERRAIN, PIECE, RAIL, START_TOOL, GAP = 0, 1, 2, 3, 4
OBJECT_INDEX = 0x15      # EditObjectDef::index (edit_objects.h)
EDITOR_STATE = 2         # g_currentGameState: 0 front end, 1 level, 2 editor
DISC = os.path.join(zpaths.DM2_WORKSPACE, 'recomp', 'disc-root')
CELL_M = 5.0


def catalog():
    """OBJECT.TXT: categories in the roll's order, objects by catalog index (file order), each its footprint."""
    t = open(os.path.join(DISC, 'ASSETS', 'EDITOR', 'OBJECT.TXT'), encoding='latin1').read().replace('\r', '')
    cats = re.findall(r'CAT \{\s*NAME "([^"]*)"', t.split('CATAGORIES', 1)[1].split('\nEND', 1)[0])
    objs = []
    for o in re.findall(r'OBJECT \{(.*?)\}', t.split('\nOBJECTS', 1)[1], re.S):
        name = re.search(r'HASH "([^"]*)"', o).group(1)
        objs.append(dict(name=re.sub('(?i)^editobj_', '', name), cat=re.search(r'CAT "([^"]*)"', o).group(1),
                         x=int(re.search(r'\bX (\d+)', o).group(1)), y=int(re.search(r'\bY (\d+)', o).group(1))))
    return cats, objs


CATS, OBJS = catalog()
INDEX = {o['name']: i for i, o in enumerate(OBJS)}


class Editor:
    def __init__(self, run):
        self.run = run

    def u8(self, at):
        return self.run.mem(at, 1)[0]

    def s8(self, at):
        return struct.unpack('<b', self.run.mem(at, 1))[0]

    def state(self):
        return self.u8(SYM['g_currentGameState'])

    def cursor(self):
        return self.s8(PE + CURSOR_ROW), self.s8(PE + CURSOR_COLUMN)

    def tool(self):
        return self.u8(PE + TOOL)

    def entry(self):
        e = self.run.u32(SYM['g_currentToolEntry'])
        if not e:
            return None
        return self.u8(e + 8), self.u8(e + 9), self.run.mem(e + 0xA, 0x1A).split(b'\0')[0].decode('latin1')

    def held(self):
        return self.u8(SYM['g_heldObject']), self.u8(SYM['g_heldRotation'])     # the catalog index, quarter turns

    def cell(self, row, col):
        at = PE + CELLS + (row * 64 + col) * CELL_BYTES
        level = self.s8(at)
        obj = self.run.u32(at + 4)
        if not obj:
            return level, None, None
        return level, self.u8(obj + 8), self.u8(obj + 0xA)

    def size(self):
        return self.u8(PE + ROWS), self.u8(PE + COLUMNS)

    def capacity(self):
        used, cap = struct.unpack('<2f', self.run.mem(SYM['g_parkCapacityUsed'], 8))
        return used, cap

    def rails(self):
        return self.run.u32(SYM['g_railCount']), self.u8(SYM['g_railSlot']), self.u8(SYM['g_railType'])

    def start(self):
        return tuple(self.run.mem(PE + START_OFFSET, 4))


class Bot:
    """A generator of pads, one a tick; `st` is the tick's dm2.State."""

    def __init__(self, run, seed=1):
        self.run = run
        self.st = None
        self.ed = Editor(run)
        self.notes = {}
        self.log = []

    def tally(self, what):
        self.notes[what] = self.notes.get(what, 0) + 1


def say(bot, *what):
    print('%6d' % bot.st.tick, *what, flush=True)


def shot(bot, what):
    if SHOTS:
        print('shot', what, bot.run.shot(what), flush=True)


def wait(n, p=NOTHING):
    for _ in range(n):
        yield p


def tap(button, hold=6, gap=30):
    yield from wait(hold, pad(button))
    yield from wait(gap)


def until(bot, cond, limit):
    for _ in range(limit):
        if cond():
            return True
        yield NOTHING
    return False


def enter(bot, theme=0):
    """Main menu (cursor on PROQUEST): PARK EDITOR (fourth), NEW PARK, the theme roll (RIGHT a theme), the editor."""
    ed = bot.ed
    for _ in range(3):
        yield from tap('DOWN', gap=30)
    yield from tap('CROSS', gap=240)
    shot(bot, 'park-menu')
    yield from tap('CROSS', gap=300)
    shot(bot, 'themes')
    for _ in range(theme):
        yield from tap('RIGHT', gap=40)
    yield from tap('CROSS', gap=60)
    ok = yield from until(bot, lambda: ed.state() == EDITOR_STATE and bot.st.mode == 0 and ed.size()[0] > 0, 3000)
    yield from wait(300)
    shot(bot, 'editor')
    say(bot, 'editor:', 'yes' if ok else 'NO', 'state', ed.state(), 'mode', bot.st.mode, 'size', ed.size(), 'theme',
        ed.u8(PE + THEME), 'cursor', ed.cursor(), 'tool', ed.tool(), 'entry', ed.entry(), 'capacity', ed.capacity())
    return ok


def goto(bot, row, col, button=None, limit=80):
    """The cursor to (row, col) a press at a time (heading 0: UP row-1, DOWN row+1, LEFT column+1, RIGHT column-1)."""
    ed = bot.ed
    for _ in range(limit):
        r, c = ed.cursor()
        if (r, c) == (row, col):
            return True
        b = ('UP' if r > row else 'DOWN') if r != row else ('RIGHT' if c > col else 'LEFT')
        held = (button,) if button else ()
        yield from wait(6, pad(b, *held))
        yield from wait(12, pad(*held))
    say(bot, 'goto %d,%d: at %s' % (row, col, ed.cursor()))
    return False


def roll_to(bot, tool, category=None, limit=24):
    """R1 along the tool roll until its entry is TOOL (and CATEGORY: the catalog's category index)."""
    ed = bot.ed
    for _ in range(limit):
        e = ed.entry()
        if e and e[0] == tool and (category is None or e[1] == category):
            return True
        yield from tap('R1', gap=40)
    say(bot, 'roll: no tool %d category %s (at %s)' % (tool, category, ed.entry()))
    return False


def place(bot, name, row, col, turns=0, zone=''):
    """OBJECT.TXT's NAME at (row, col), turned TURNS quarter turns (CIRCLE until g_heldRotation says so), CROSS; the
    cell read back."""
    ed = bot.ed
    want = INDEX[name]
    ok = yield from roll_to(bot, PIECE, CATS.index(OBJS[want]['cat']))
    for _ in range(40):
        if ed.held()[0] == want:
            break
        yield from tap('R2', gap=30)
    for _ in range(8):
        if ed.held()[1] == turns % 4:
            break
        yield from tap('CIRCLE', gap=30)
    yield from goto(bot, row, col)
    used0 = ed.capacity()[0]
    yield from tap('CROSS', gap=60)
    level, obj, rot = ed.cell(row, col)
    used = ed.capacity()[0]
    placed = obj == want
    say(bot, 'place %-28s at %2d,%2d turns %d (held %s): %s, capacity %.0f (+%.0f)' % (
        name, row, col, turns, ed.held(), 'placed' if placed else 'NOT placed (cell %s %s)' % (obj, rot), used, used - used0))
    bot.log.append(dict(zone=zone, name=name, row=row, col=col, turns=turns, placed=placed, rotation=rot, cost=used - used0))
    if not placed and bot.st.mode == 3:        # a stop frame ("Too many Objects!"): its CROSS
        yield from tap('CROSS', gap=60)
    return placed


def terrain(bot, r0, c0, r1, c1, levels, zone=''):
    """The terrain tool (the roll's first: SELECT): CROSS held from (r0, c0) to (r1, c1) stretches the selection, let go
    the tape opens; UP or DOWN a level a press; CIRCLE ends."""
    ed = bot.ed
    yield from tap('SELECT', gap=40)
    yield from goto(bot, r0, c0)
    yield from wait(8, pad('CROSS'))
    yield from goto(bot, r1, c1, button='CROSS')
    yield from wait(6, pad('CROSS'))
    yield from wait(30)
    for _ in range(abs(levels)):
        yield from tap('UP' if levels > 0 else 'DOWN', hold=4, gap=12)
    yield from wait(20)
    yield from tap('CIRCLE', gap=40)
    got = [ed.cell(r, c)[0] for r in range(min(r0, r1), max(r0, r1) + 1) for c in range(min(c0, c1), max(c0, c1) + 1)]
    say(bot, 'terrain %d,%d-%d,%d %+d levels: cell levels %s' % (r0, c0, r1, c1, levels, sorted(set(got))))
    bot.log.append(dict(zone=zone, terrain=(r0, c0, r1, c1), levels=levels, got=sorted(set(got))))


def rail(bot, row, col, slot, rtype=0, zone=''):
    """The rail tool: (row, col), CIRCLE to the slot (the Flat group's: 0-3 the cell's edges, 4 and 5 the centre turned
    1 and 0, 6 and 7 across), R2 to the type (0 ShortRail, 1 TallRail, 2 ShortWall, 3 TallWall), CROSS."""
    ed = bot.ed
    ok = yield from roll_to(bot, RAIL)
    yield from goto(bot, row, col)
    yield from wait(10)
    for _ in range(10):
        if ed.rails()[1] == slot:
            break
        yield from tap('CIRCLE', gap=30)
    for _ in range(6):
        if ed.rails()[2] == rtype:
            break
        yield from tap('R2', gap=30)
    n0 = ed.rails()[0]
    yield from tap('CROSS', gap=60)
    n, s, t = ed.rails()
    placed = n == n0 + 1
    say(bot, 'rail at %2d,%2d slot %d type %d: %s (rails %d)' % (row, col, s, t, 'placed' if placed else 'NOT placed', n))
    bot.log.append(dict(zone=zone, rail=(row, col), slot=s, type=t, placed=placed))
    return placed


def start(bot, row, col, turns):
    ed = bot.ed
    ok = yield from roll_to(bot, START_TOOL)
    yield from goto(bot, row, col)
    for _ in range(8):
        if ed.start()[2] == turns % 4 and ed.start()[3]:
            break
        yield from tap('CIRCLE', gap=30)
    yield from tap('CROSS', gap=60)
    say(bot, 'start: %s' % (ed.start(),))
    bot.log.append(dict(start=ed.start()))


def view(bot, what, pitch=1.0, zoom=1.0, frames=90):
    """The camera tilted (left stick Y) and zoomed (right stick Y) for a picture, then back."""
    if not SHOTS:
        return
    yield from wait(frames, pad(ly=-pitch, ry=zoom))
    yield from wait(30)
    shot(bot, what)
    yield from wait(frames, pad(ly=pitch, ry=-zoom))
    yield from wait(20)


def session(name, program, ticks=60000, env=None, script_extra=None):
    import dm2routes
    menu = dm2routes.Script()
    e = {'ZX_HEADLESS': '1'}
    e.update(env or {})
    cfg = dm2.run_config(name, menu.text(), env=e, max_secs=max(900, ticks // 12), gs=SHOTS)
    rec = core.Recorder()
    with core.Run(cfg) as run:
        bot = Bot(run)
        gen = program(bot)

        def policy(st):
            bot.st = st
            if st.script < menu.t + 30:
                return NOTHING
            try:
                return next(gen)
            except StopIteration:
                return None

        core.play(run, dm2.read, policy, max_ticks=ticks, recorder=rec)
    return bot, run


def probe(name):
    """Orientation: a quarter pipe, a kicker, a roll-in and rails at each turn, and a picture from above."""
    def program(bot):
        ok = yield from enter(bot, int(os.environ.get('THEME', '0')))
        if not ok:
            return
        for t in range(4):
            yield from place(bot, 'qpipe_qpipe4', 4, 4 + 3 * t, t)
            yield from place(bot, 'kicker_4', 7, 4 + 3 * t, t)
            yield from place(bot, 'rollin_4', 10, 4 + 3 * t, t)
        yield from rail(bot, 14, 6, 5)
        yield from rail(bot, 14, 9, 4)
        yield from rail(bot, 14, 12, 0)
        yield from rail(bot, 14, 15, 1)
        yield from goto(bot, 9, 10)
        yield from view(bot, name + '-top', frames=150)
        say(bot, 'probe done; capacity', bot.ed.capacity())

    bot, run = session(name, program)
    core.save_json(os.path.join(dm2.SCRATCH, 'measure', name + '.probe.json'), bot.log)


def probe3(name):
    """Orientation, close up: each piece at each turn on (8, 10), the cursor three rows behind (+row), the camera
    pitched low, a picture; SQUARE erases it again."""
    pieces = os.environ.get('PIECES', 'qpipe_qpipe4,kicker_4,rollin_4,funbox_SR1,jumpbox_hip1,dirt_BIGlip,ramp_4').split(',')

    def program(bot):
        ok = yield from enter(bot, 0)
        if not ok:
            return
        yield from wait(120, pad(ly=1.0))          # pitched down to the ground
        yield from wait(20)
        for piece in pieces:
            for t in range(4):
                yield from place(bot, piece, 8, 10, t)
                yield from goto(bot, 11, 10)
                yield from wait(40)
                shot(bot, '%s-%s-%d' % (name, piece, t))
                yield from goto(bot, 8, 10)
                yield from tap('SQUARE', gap=40)

    bot, run = session(name, program)


# ---- the park: six zones, the AI park's layout (aipark.LAYOUT) on Warehouse's floor --
# AI's cell (x, z) is DM2's (row x + 2, column z + 2) on the 20 x 20 floor (rows and columns 2 .. 21; 5 m cells); the
# d-pad and the camera agree (UP row-1 as AI's x-1, LEFT column+1 as AI's z+1). What a piece faces at each quarter
# turn (probe3's pictures, the camera on +row): 0 +row, 1 -column, 2 -row, 3 +column; AI's turn t is DM2's (1 - t) % 4.
# Heights are metres (a level a metre: a cell's ground is -level; Warehouse's floor stands at level 10); the pieces'
# names say their height (qpipe_qpipe4: "the medium, 4 meter quarter pipe"). Straight rails and walls are the rail
# tool's on bare cells (the Flat group: slot 5 a centre rail along the columns, 4 along the rows; type 0 ShortRail,
# 1 TallRail, 2 ShortWall, 3 TallWall).
WAREHOUSE = 0
TERRAIN = [('tower', (2, 9, 3, 10), 4)]
LAYOUT = [
    # zone, object, row, column, turns
    ('tower', 'rollin_4', 4, 9, 0),
    ('tower', 'rollin_4', 4, 10, 0),
    ('tower', 'kicker_4', 7, 9, 2),
    ('tower', 'kicker_4', 7, 10, 2),
    ('tower', 'ramp_4', 2, 12, 3),
    ('tower', 'ramp_4', 3, 12, 3),
    ('plaza', 'funbox_SR1', 12, 10, 1),
    ('plaza', 'funbox_pyramid', 9, 6, 0),
    ('plaza', 'stairs_quad', 11, 13, 0),
    ('pools', 'bowl_qpipe', 5, 3, 0),
    ('pools', 'bowl_rollout', 9, 3, 0),
    ('hangar', 'qpipe_qpipe4', 15, 6, 1),
    ('hangar', 'qpipe_qpipe4', 16, 6, 1),
    ('hangar', 'qpipe_qpipe4', 15, 3, 3),
    ('hangar', 'qpipe_qpipe4', 16, 3, 3),
    ('yard', 'jumpbox_hip1', 15, 13, 0),
    ('yard', 'jumpbox_hipwall1', 18, 13, 0),
    ('yard', 'dirt_BIGlip', 13, 19, 2),
    ('yard', 'dirt_BIGlip', 17, 19, 0),
    ('lawn', 'specialtyrails_bow', 5, 17, 1),
    ('lawn', 'funbox_SR2', 7, 18, 0),
]
RAILS = [('plaza', 9, 12, 5, 0), ('plaza', 9, 13, 5, 0)] + \
    [('lawn', r, 15, 4, 0) for r in range(4, 9)] + [('hangar', r, 8, 4, 3) for r in range(13, 17)]
START = (2, 9, int(os.environ.get('START_TURNS', '0')))


def keyboard_end(bot):
    """A DM2 keyboard (4 rows of 12, SPC DEL END under them) from A: A typed, LEFT (wraps to the last column), DOWN
    four times to END (dm2routes.keyboard_end), END."""
    yield from tap('CROSS', gap=30)
    yield from tap('LEFT', gap=20)
    for _ in range(4):
        yield from tap('DOWN', gap=20)
    yield from tap('CROSS', gap=200)


def save_park(bot):
    """The pause (CONTINUE, SAVE PARK, TEST RUN, LIGHT OPTIONS, CLEAR ALL, EXIT): SAVE PARK, SELECT SLOT, memory card
    slot 1 (its parks read), the first row, the park's name and author keyboards, SAVE COMPLETE; back to the editor."""
    ed = bot.ed
    yield from tap('START', gap=120)
    yield from tap('DOWN', gap=30)
    shot(bot, 'save-0')
    yield from tap('CROSS', gap=600)
    shot(bot, 'save-1')
    yield from tap('CROSS', gap=1200)
    shot(bot, 'save-2')
    yield from tap('CROSS', gap=600)
    shot(bot, 'save-3')
    yield from keyboard_end(bot)
    shot(bot, 'save-4')
    yield from keyboard_end(bot)
    for _ in range(9000):                       # SAVE COMPLETE: the frame's step 9 (card_frame.h)
        if bot.run.mem(SYM['g_uiFrameStep'], 1)[0] == 9:
            break
        yield NOTHING
    yield from wait(120)
    shot(bot, 'save-5')
    say(bot, 'save: frame kind %d step %d' % (bot.run.mem(SYM['g_uiFrameKind'], 1)[0], bot.run.mem(SYM['g_uiFrameStep'], 1)[0]))
    yield from tap('CROSS', gap=600)
    yield from tap('UP', gap=30)
    yield from tap('CROSS', gap=120)
    shot(bot, 'save-6')


def build(name):
    out = {}

    def program(bot):
        ok = yield from enter(bot, WAREHOUSE)
        if not ok:
            return
        ed = bot.ed
        for zone, rect, levels in TERRAIN:
            yield from terrain(bot, *rect, levels, zone=zone)
        for zone, obj, row, col, turns in LAYOUT:
            yield from place(bot, obj, row, col, turns, zone=zone)
        for zone, row, col, slot, rtype in RAILS:
            yield from rail(bot, row, col, slot, rtype, zone=zone)
        yield from start(bot, *START)
        out['capacity'] = ed.capacity()
        out['rails'] = ed.rails()[0]
        yield from goto(bot, 11, 11)
        yield from view(bot, name + '-top', frames=150)
        say(bot, 'built: %d of %d pieces, %d of %d rails, capacity %s' % (
            sum(1 for p in bot.log if p.get('placed') and 'name' in p), len(LAYOUT),
            sum(1 for p in bot.log if p.get('placed') and 'rail' in p), len(RAILS), out['capacity']))
        yield from save_park(bot)
        yield from wait(200)

    bot, run = session(name, program, ticks=90000)
    out['log'] = bot.log
    card = []
    for root, _, files in os.walk(os.path.join(run.dir, 'mc0')):
        for f in files:
            card.append((os.path.relpath(os.path.join(root, f), run.dir), os.path.getsize(os.path.join(root, f))))
    out['card'] = card
    core.save_json(os.path.join(dm2.SCRATCH, 'measure', name + '.build.json'), out)
    print('card:', card)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'build':
        build(a[1])
    elif a and a[0] == 'probe3':
        probe3(a[1])
    elif a and a[0] == 'probe':
        probe(a[1])
    else:
        print(__doc__)
