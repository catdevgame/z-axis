#!/usr/bin/env python3
"""Aggressive Inline: the park players' test park built in the game's own
Park Editor, saved the game's way, and skated.

    aipark.py catalog NAME                the editor's piece catalog (name, group, footprint) read from the game
    aipark.py build NAME [THEME]          the six-zone park built (Repository, theme 1, by default), a Test Run,
                                          saved (Save Park: a native file, <saves>/Parks/Park NNN.sav)
    aipark.py ride NAME PARK TICKS [SEED] the saved park (PARK: its Parks folder) skated from Free Skate's Editor
                                          Level, the playbot's numbers (distance, grinds, rail metres, air, tricks,
                                          score) in $PLAYAI/measure/NAME.report.json

The editor is closed on the game's own state: the cursor cell is the selection's (EditSelection::cursorCell), the
tool the tool set's (EditToolSet::currentTool), the piece the Object Tool's (EditObjectToolPart::pieceDef), so a
placement is a d-pad walk to the cell, the tool roll to the piece's group, R2 to the piece, O to turn it, X, and the
park's piece count read back. SHOTS=1: pictures at the steps. Scratch: $PLAYAI.
"""
import json
import math
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ai  # noqa: E402
import core  # noqa: E402
import memuse  # noqa: E402
import parkmap  # noqa: E402
from core import pad  # noqa: E402
from ai import tap, wait, until, new_panel, vmenu, set_cursor  # noqa: E402

SHOTS = os.environ.get('SHOTS') == '1'
EDIT_CORE = ai.SYM['g_parkEditorCore']
# EditCore (zge/editor/edit_core.h), EditToolSet and PieceDef (edit_records.h), EditSelection (park_selection.h),
# PieceCatalog (park_catalog.h), EditPart (park_classes.h)
CORE_CATALOG, CORE_TOOLSET, CORE_THEME_INDEX = 0x10, 0x14, 0x1C
TOOLSET_CURRENT, TOOLSET_SELECTION = 0x48, 0x5C
SELECTION_CURSOR = 0x5C
CATALOG_COUNT, CATALOG_PIECES = 0x04, 0x08
PIECE_NAME, PIECE_CATEGORY, PIECE_SECRET, PIECE_CELLS = 0x08, 0x6C, 0x70, 0x74
OBJECT_TOOL_PIECE = 0x04
GROUPS = ('Terrain', 'Ramp', 'Spine', 'Kicker', 'QuaterPipe', 'Bowl', 'FunBox', 'BonusPieces', 'JumpBox', 'Pole',
          'Rail', 'RollIn', 'Wall', 'SubBox', 'RailTool', 'StartTool')
# the tool roll's piece groups are the catalog's categories without SystemFlag (OBJECT.OBJ): Ramp 1 ... SubBox 15
CATEGORY_OF_GROUP = {1: 1, 2: 2, 3: 3, 4: 4, 5: 5, 6: 6, 7: 7, 8: 9, 9: 10, 10: 11, 11: 13, 12: 14, 13: 15}


def cstr(run, at, n=0x40):
    b = run.mem(at, n)
    return b.split(b'\0', 1)[0].decode('latin1')


def edit_part(run, data, class_name):
    """editObjectPart: the object's part of the class (park_classes.cpp)."""
    if not data:
        return 0
    part = run.u32(data - 0x10)
    for _ in range(16):
        if not part:
            return 0
        if cstr(run, run.u32(part + 8) + 4) == class_name:
            return run.u32(part + 0xC)
        part = run.u32(part + 4)
    return 0


class Editor:
    """The editor's state, read from the game."""

    def __init__(self, run):
        self.run = run

    @property
    def core(self):
        return self.run.u32(EDIT_CORE)

    @property
    def toolset(self):
        c = self.core
        return self.run.u32(c + CORE_TOOLSET) if c else 0

    def tool(self):
        t = self.toolset
        return self.run.u32(t + TOOLSET_CURRENT) if t else 0

    def cursor(self):
        t = self.toolset
        if not t:
            return None
        sel = self.run.u32(t + TOOLSET_SELECTION)
        if not sel:
            return None
        return struct.unpack('<3i', self.run.mem(sel + SELECTION_CURSOR, 12))

    def selection_position(self):
        """EditSelection::position: where the selection box is drawn (eases towards the cursor cell), in the world."""
        t = self.toolset
        sel = self.run.u32(t + TOOLSET_SELECTION) if t else 0
        return struct.unpack('<3f', self.run.mem(sel, 12)) if sel else None

    def tool_class(self):
        """The class names of the current tool's parts, most derived first."""
        data = self.tool()
        if not data:
            return []
        names, part = [], self.run.u32(data - 0x10)
        for _ in range(8):
            if not part:
                break
            names.append(cstr(self.run, self.run.u32(part + 8) + 4))
            part = self.run.u32(part + 4)
        return names

    def piece(self):
        """The Object Tool's piece (name, category), or None on another tool."""
        part = edit_part(self.run, self.tool(), 'EditObjectTool')
        if not part:
            return None
        p = self.run.u32(part + OBJECT_TOOL_PIECE)
        if not p:
            return None
        return cstr(self.run, p + PIECE_NAME, 0x20), self.run.u32(p + PIECE_CATEGORY)

    def catalog(self):
        c = self.core
        cat = self.run.u32(c + CORE_CATALOG)
        n = self.run.u32(cat + CATALOG_COUNT)
        table = self.run.u32(cat + CATALOG_PIECES)
        out = []
        for i in range(min(n, 400)):
            p = self.run.u32(table + 4 * i)
            if not p:
                continue
            cx, cz, cy = struct.unpack('<3I', self.run.mem(p + PIECE_CELLS, 12))
            ox, oy, oz = struct.unpack('<3i', self.run.mem(p + PIECE_CELLS + 12, 12))
            out.append(dict(i=i, name=cstr(self.run, p + PIECE_NAME, 0x20), category=self.run.u32(p + PIECE_CATEGORY),
                            secret=struct.unpack('<i', self.run.mem(p + PIECE_SECRET, 4))[0], cells=(cx, cz, cy),
                            origin=(ox, oy, oz)))
        return out


def shot(bot, what):
    if SHOTS:
        print('shot', what, 'tick', bot.s.tick, bot.run.shot(what), flush=True)


def say(bot, *what):
    print('%6d' % bot.s.tick, *what, flush=True)


def enter(bot, theme=1):
    """Main menu > PARK EDITOR, the first skater (a new skater's name entry: END), New Park, theme THEME."""
    yield from ai.boot(bot)
    for _ in range(6):
        yield from tap(bot, 'RIGHT', hold=6, gap=30)
    yield from tap(bot, 'LEFT', hold=6, gap=30)
    shot(bot, 'menu')
    p0 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=60)
    p1 = yield from new_panel(bot, p0)
    yield from tap(bot, 'CROSS', gap=60)
    p2 = yield from new_panel(bot, p1)
    yield from wait(bot, 30)
    if bot.s.cursor == ai.NAME_ENTRY_END:
        yield from tap(bot, 'CROSS', gap=60)
        p2 = yield from new_panel(bot, p2)
    shot(bot, 'pe-menu')
    say(bot, 'park editor menu: panel %#x cursor %d' % (bot.s.panel, bot.s.cursor))
    yield from vmenu(bot, 0)                       # New Park
    p3 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=60)
    yield from new_panel(bot, p3)
    shot(bot, 'themes')
    say(bot, 'themes: panel %#x cursor %d' % (bot.s.panel, bot.s.cursor))
    for _ in range(theme):                         # the theme roll (its panel cursor stays 0): RIGHT a theme
        yield from tap(bot, 'RIGHT', hold=6, gap=40)
    yield from tap(bot, 'CROSS', gap=60)
    bot.pe_at = 0
    ed = Editor(bot.run)
    ok = yield from until(bot, lambda s: s.mode == ai.PLAY and ed.core != 0 and ed.cursor() is not None, 6000)
    yield from wait(bot, 300)
    shot(bot, 'editor')
    say(bot, 'editor:', 'yes' if ok else 'NO', 'mode', bot.s.mode, 'core %#x' % ed.core, 'cursor', ed.cursor(),
        'tool', ed.tool_class())
    return ok


G_PARK_COST = ai.SYM['g_parkCost']
DPAD = {'UP': (-1, 0), 'DOWN': (1, 0), 'RIGHT': (0, -1), 'LEFT': (0, 1)}   # the default camera's: (x, z) a tap


def cost(bot):
    return struct.unpack('<i', bot.run.mem(G_PARK_COST, 4))[0]


def goto(bot, x, z, button=None, limit=60):
    """The cursor to cell (x, z), a tap at a time, read back (BUTTON held throughout: the terrain's drag)."""
    ed = Editor(bot.run)
    for _ in range(limit):
        c = ed.cursor()
        if c is None:
            return False
        if (c[0], c[2]) == (x, z):
            return True
        if c[0] != x:
            b = 'UP' if c[0] > x else 'DOWN'
        else:
            b = 'RIGHT' if c[2] > z else 'LEFT'
        held = (button,) if button else ()
        yield from wait(bot, 6, pad(b, *held))
        yield from wait(bot, 14, pad(*held))
    return False


def roll(bot, g):
    """The tool roll to entry g (R1 / L1 the shorter way), checked on the tool's class and the piece's category."""
    at = getattr(bot, 'pe_at', 0)
    fwd = (g - at) % 16
    button, n = ('R1', fwd) if fwd <= 8 else ('L1', 16 - fwd)
    for _ in range(n):
        yield from tap(bot, button, hold=6, gap=50)
    yield from wait(bot, 20)
    bot.pe_at = g
    ed = Editor(bot.run)
    want = CATEGORY_OF_GROUP.get(g)
    if want is not None:
        p = ed.piece()
        if not p or p[1] != want:
            say(bot, 'roll: group %d wanted category %d, have %s %s' % (g, want, ed.tool_class(), p))
            return False
    return True


def choose(bot, g, name):
    ok = yield from roll(bot, g)
    ed = Editor(bot.run)
    for _ in range(24):
        p = ed.piece()
        if p and p[0] == name:
            return True
        yield from tap(bot, 'R2', hold=6, gap=30)
    say(bot, 'choose: no %r in group %d' % (name, g))
    return False


def place(bot, g, name, x, z, turns=0, log=None):
    """A piece: group g's NAME at cell (x, z), turned TURNS times (O), placed (X); its cost read back."""
    ok = yield from choose(bot, g, name)
    if not ok:
        return False
    for _ in range(turns % 4):
        yield from tap(bot, 'CIRCLE', hold=6, gap=30)
    ok = yield from goto(bot, x, z)
    c0 = cost(bot)
    yield from tap(bot, 'CROSS', hold=6, gap=60)
    yield from wait(bot, 10)
    c1 = cost(bot)
    placed = c1 != c0
    say(bot, 'place %-24s at %2d,%2d turns %d: %s cost %d (+%d)' % (name, x, z, turns, 'placed' if placed else 'REFUSED', c1, c1 - c0))
    if log is not None:
        log.append(dict(group=g, name=name, x=x, z=z, turns=turns, placed=placed, cost=c1 - c0))
    # the piece stays on the cursor; turn it back so the next one starts square
    for _ in range((4 - turns % 4) % 4):
        yield from tap(bot, 'CIRCLE', hold=6, gap=30)
    return placed


def terrain(bot, x0, z0, x1, z1, feet, log=None):
    """The Terrain Tool: X held from (x0, z0) to (x1, z1) drags a rectangle, let go it is chosen; UP / DOWN raise or
    lower it a foot a press (repeating every 4 frames held); O drops it."""
    yield from roll(bot, 0)
    ok = yield from goto(bot, x0, z0)
    yield from wait(bot, 8, pad('CROSS'))
    yield from goto(bot, x1, z1, button='CROSS')
    yield from wait(bot, 6, pad('CROSS'))
    yield from wait(bot, 20)
    b = 'UP' if feet > 0 else 'DOWN'
    for _ in range(abs(feet)):
        yield from tap(bot, b, hold=3, gap=6)
    yield from wait(bot, 20)
    yield from tap(bot, 'CIRCLE', hold=6, gap=40)
    say(bot, 'terrain %d,%d-%d,%d %+d ft' % (x0, z0, x1, z1, feet))
    if log is not None:
        log.append(dict(terrain=(x0, z0, x1, z1), feet=feet))


def overview(bot, what):
    """A picture of the whole park: the camera zoomed out (right stick), then back."""
    if not SHOTS:
        return
    for i, (lx, ly, rx, ry) in enumerate(((0, -1, 0, 0), (0, 1, 0, 0), (0, 1, 0, 0), (0, 0, 0, 1), (0, 0, 0, -1), (0, 0, 0, -1))):
        yield from wait(bot, 60, pad(lx=lx, ly=ly, rx=rx, ry=ry))
        yield from wait(bot, 30)
        shot(bot, '%s-%d' % (what, i))


def probe2(name):
    """Orientation: pieces at each turn in rows the camera looks along, a zoomed-out picture."""
    log = []

    def behaviour(bot):
        ok = yield from enter(bot, int(os.environ.get('THEME', '3')))
        if not ok:
            return
        ed = Editor(bot.run)
        say(bot, 'theme index', bot.run.u32(ed.core + CORE_THEME_INDEX), 'cursor', ed.cursor())
        rows = ((4, "15' Quarter Pipe"), (11, "15' Roll-In"), (3, "10' Kicker"), (10, 'Centered Rail'))
        for r, (g, piece) in enumerate(rows):
            for t in range(4):
                yield from place(bot, g, piece, 2 + 3 * r, 2 + 3 * t, turns=t, log=log)
        yield from goto(bot, 15, 6)
        if SHOTS:
            for i in range(3):
                yield from wait(bot, 60, pad(ry=1.0))
                yield from wait(bot, 30)
                shot(bot, '%s-out%d' % (name, i))
        yield from wait(bot, 60)

    run_session(name, behaviour, 30000)
    core.save_json(os.path.join(ai.SCRATCH, 'measure', name + '.probe.json'), log)


def probe3(name):
    """Orientation, close up: each piece at each turn on cell (8, 8), a picture from two cells behind (the camera looks
    along -x), deleted (Square) again."""
    log = []
    pieces = [(4, "15' Quarter Pipe"), (11, "15' Roll-In"), (3, "10' Kicker"), (10, 'Centered Rail'), (12, 'Center Wall'),
              (6, 'Fun Box A'), (2, "12' Spine"), (8, 'Jump Box C'), (13, "5' Wedge")]
    only = os.environ.get('PIECES')
    if only:
        pieces = [pieces[int(i)] for i in only.split(',')]

    def behaviour(bot):
        ok = yield from enter(bot, 3)
        if not ok:
            return
        for g, piece in pieces:
            for t in range(4):
                yield from place(bot, g, piece, 8, 8, turns=t, log=log)
                yield from goto(bot, 11, 8)
                yield from wait(bot, 40)
                shot(bot, '%s-%s-%d' % (name, piece.replace("'", '').replace(' ', ''), t))
                yield from goto(bot, 8, 8)
                yield from tap(bot, 'SQUARE', hold=6, gap=40)
                yield from wait(bot, 10)
        yield from wait(bot, 60)

    run_session(name, behaviour, 60000)
    core.save_json(os.path.join(ai.SCRATCH, 'measure', name + '.probe.json'), log)


# ---- the park: six zones on the Repository's 16 x 16 grid -----------------------
# Cells (x, z); the default camera looks along -x (d-pad UP), RIGHT is -z. A piece's turns (O presses) and what it
# faces (probe3's pictures): 0 -z, 1 +x, 2 +z, 3 -x; a quarter pipe, kicker or roll-in faces the way a skater comes
# at it (rides up it going the other way, rolls down a roll-in going that way); walls and rails at 0 and 2 run along x,
# at 1 and 3 along z. Footprints (catalog): Fun Box A 1 x 4 cells along z at turn 0, a 15' Roll-In or Kicker 1 x 2,
# a 12' Bowl 4 x 4, a 10' Half Bowl 4 x 2, Jump Box C 4 x 3, Hip Wall Jump Box 3 x 2.
REPOSITORY = 3            # RIGHT taps on the theme roll: Repository (core theme index 1, 16 x 16, start 8, 8)
TERRAIN = [('tower deck', (0, 7, 1, 8), 15)]
LAYOUT = [
    # zone, group, piece, x, z, turns
    ('tower', 11, "15' Roll-In", 2, 7, 1),
    ('tower', 11, "15' Roll-In", 2, 8, 1),
    ('tower', 3, "15' Kicker", 5, 7, 3),
    ('tower', 3, "15' Kicker", 5, 8, 3),
    ('plaza', 6, 'Fun Box A', 10, 8, 0),
    ('plaza', 6, 'Pyramid B', 7, 4, 0),
    ('plaza', 10, 'Centered Rail', 7, 10, 1),
    ('plaza', 10, 'Centered Rail', 7, 11, 1),
    ('plaza', 13, "5' Wedge", 9, 11, 1),
    ('pools', 5, "12' Bowl", 3, 2, 0),
    ('pools', 5, "10' Half Bowl", 7, 1, 0),
    ('hangar', 4, "10' Quarter Pipe", 13, 4, 0),
    ('hangar', 4, "10' Quarter Pipe", 14, 4, 0),
    ('hangar', 4, "10' Quarter Pipe", 13, 1, 2),
    ('hangar', 4, "10' Quarter Pipe", 14, 1, 2),
    ('hangar', 12, 'Center Wall', 11, 6, 0),
    ('hangar', 12, 'Center Wall', 12, 6, 0),
    ('hangar', 12, 'Center Wall', 13, 6, 0),
    ('hangar', 12, 'Center Wall', 14, 6, 0),
    ('yard', 8, 'Jump Box C', 13, 11, 0),
    ('yard', 8, 'Hip Wall Jump Box', 12, 14, 0),
    ('yard', 3, "10' Kicker", 15, 15, 1),
    ('yard', 3, "10' Kicker", 11, 15, 3),
    ('lawn', 10, 'Centered Rail', 2, 13, 0),
    ('lawn', 10, 'Centered Rail', 3, 13, 0),
    ('lawn', 10, 'Centered Rail', 4, 13, 0),
    ('lawn', 10, 'Centered Rail', 5, 13, 0),
    ('lawn', 10, 'Centered Rail', 6, 13, 0),
    ('lawn', 10, 'Center Bow Rail', 3, 15, 0),
    ('lawn', 6, 'Fun Box B', 5, 15, 0),
    ('tower', 1, "15' Ramp", 0, 10, 2),                # the way up: banks onto the deck from the lawn side
    ('tower', 1, "15' Ramp", 1, 10, 2),
]
START = (0, 7, int(os.environ.get('START_TURNS', '3')))     # on the tower's deck, facing its roll-ins
PE_KEYS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-+!? \x7f\x03"    # the card dialogs' keyboard: 9 keys a row, END last


def key(bot, k):
    """The keyboard's cursor to key k: across first, then down (routes.py pe_key)."""
    for _ in range(40):
        c = bot.s.cursor
        if c == k or c < 0 or c > 255:
            return c == k
        cr, cc = divmod(c, 9)
        kr, kc = divmod(k, 9)
        b = ('RIGHT' if kc > cc else 'LEFT') if kc != cc else ('DOWN' if kr > cr else 'UP')
        yield from tap(bot, b, hold=6, gap=12)
    return False


def type_text(bot, text):
    for ch in text:
        yield from key(bot, PE_KEYS.index(ch))
        yield from tap(bot, 'CROSS', hold=6, gap=15)
    yield from key(bot, len(PE_KEYS) - 1)
    yield from tap(bot, 'CROSS', hold=6, gap=60)


def pause_item(bot, k):
    """The editor's pause (Continue, Test Run, Save Park, Load Park, Clear Park, Sound Options, Exit Editor)."""
    yield from tap(bot, 'START', hold=6, gap=60)
    yield from wait(bot, 30)
    yield from vmenu(bot, k)
    say(bot, 'pause item %d: cursor %d' % (k, bot.s.cursor))
    yield from tap(bot, 'CROSS', hold=6, gap=60)
    yield from wait(bot, 60)


def yes(bot):
    yield from tap(bot, 'UP', hold=6, gap=20)
    yield from tap(bot, 'CROSS', hold=6, gap=60)


def type_blind(bot, text):
    """A card dialog's keyboard (9 keys a row, A first, END last), from A: the moves counted, not read (the keyboard's
    highlight is not the panel's cursor)."""
    at = 0
    for k in [PE_KEYS.index(ch) for ch in text] + [len(PE_KEYS) - 1]:
        (r0, c0), (r1, c1) = divmod(at, 9), divmod(k, 9)
        for _ in range(abs(c1 - c0)):
            yield from tap(bot, 'RIGHT' if c1 > c0 else 'LEFT', hold=6, gap=12)
        for _ in range(abs(r1 - r0)):
            yield from tap(bot, 'DOWN' if r1 > r0 else 'UP', hold=6, gap=12)
        yield from tap(bot, 'CROSS', hold=6, gap=20)
        at = k
    yield from wait(bot, 60)


def keyboard_a_end(bot):
    """A card dialog's keyboard from its start on A: A typed, then UP (wraps to the bottom row, 7 keys) and LEFT
    (wraps to its last key, END), END. (Counted taps that cross rows land on other keys: the dialog's own cursor is not
    the panel's, so this stays to moves that wrap.)"""
    yield from tap(bot, 'CROSS', hold=6, gap=40)
    yield from tap(bot, 'UP', hold=6, gap=40)
    yield from tap(bot, 'LEFT', hold=6, gap=40)
    yield from tap(bot, 'CROSS', hold=6, gap=90)


def save_park(bot):
    """Pause > Save Park: CHOOSE PARK (NEW first), the description and author keyboards (A, END each); with native
    saves END writes <saves>/Parks/Park NNN.sav and the pause menu is back."""
    yield from pause_item(bot, 2)
    yield from wait(bot, 60)
    shot(bot, 'save-1')
    yield from tap(bot, 'CROSS', hold=6, gap=120)      # NEW
    shot(bot, 'save-2')
    yield from keyboard_a_end(bot)
    shot(bot, 'save-3')
    yield from keyboard_a_end(bot)
    yield from wait(bot, 300)
    shot(bot, 'save-4')
    for _ in range(4):
        if bot.s.mode != ai.PAUSED:
            break
        yield from tap(bot, 'TRIANGLE', hold=6, gap=40)
    say(bot, 'save done: mode', bot.s.mode)


def start_tool(bot, x, z, turns):
    """The Start Tool (roll 15): the skater's start at (x, z), turned TURNS times, placed."""
    yield from roll(bot, 15)
    for _ in range(turns % 4):
        yield from tap(bot, 'CIRCLE', hold=6, gap=30)
    yield from goto(bot, x, z)
    yield from tap(bot, 'CROSS', hold=6, gap=60)
    say(bot, 'start at %d,%d turns %d' % (x, z, turns))


def build(name):
    log = dict(terrain=[], pieces=[], world={})

    def behaviour(bot):
        ok = yield from enter(bot, REPOSITORY)
        if not ok:
            return
        ed = Editor(bot.run)
        log['theme_index'] = bot.run.u32(ed.core + CORE_THEME_INDEX)
        log['start_cursor'] = ed.cursor()
        for c in ((8, 8), (0, 0), (15, 15)):           # the cell grid in the world: the selection box at three cells
            yield from goto(bot, *c)
            yield from wait(bot, 90)
            log['world']['%d,%d' % c] = ed.selection_position()
        say(bot, 'cells in the world:', log['world'])
        for zone, rect, feet in TERRAIN:
            yield from terrain(bot, *rect, feet, log=log['terrain'])
        for zone, g, piece, x, z, turns in LAYOUT:
            yield from place(bot, g, piece, x, z, turns=turns, log=log['pieces'])
            log['pieces'][-1]['zone'] = zone
        yield from start_tool(bot, *START)
        log['start'] = START
        log['cost'] = cost(bot)
        yield from goto(bot, 15, 8)
        yield from overview(bot, name + '-park')
        say(bot, 'built: %d of %d pieces, cost %d' % (sum(p['placed'] for p in log['pieces']), len(LAYOUT), log['cost']))
        yield from save_park(bot)
        yield from wait(bot, 120)

    run = run_session(name, behaviour, 60000, env={'PS2X_SAVES': None})
    saves = []
    for root, _, files in os.walk(run.dir):
        for f in files:
            if f.endswith('.sav') or 'PARK' in f.upper():
                p = os.path.join(root, f)
                saves.append((os.path.relpath(p, run.dir), os.path.getsize(p)))
    log['saves'] = saves
    core.save_json(os.path.join(ai.SCRATCH, 'measure', name + '.build.json'), log)
    print('saves:', saves)


# ---- riding it ---------------------------------------------------------------------------------------------------
CELL_M = 4.572            # a cell, 15 ft; the selection box read in the world (build's log): cell (8, 8) at
                          # (2.286, -2.286), (0, 0) at (-34.29, 34.29): x = (cell x - 7.5) * 4.572, z = -(cell z - 7.5) * 4.572
                          # is a cell's centre (the lawn rail's grind and the spawn on the deck agree); x.5 is a border


def W(x, z, y=0.0):
    return ((x - 7.5) * CELL_M, y, -(z - 7.5) * CELL_M)


def D(dx, dz):
    """A cell-grid direction in the world."""
    size = math.hypot(dx, dz)
    return (dx / size, 0.0, -dz / size)


class FlatLevel:
    """The park as the air's landing estimate sees it (ai.air_left): flat ground, at the lowest place the skater has
    stood (+y is down)."""

    def __init__(self):
        self.floor = None

    def nearest(self, pos, reach=1):
        return (round(pos[0]), round(pos[2]))

    def world(self, node):
        return (node[0], self.floor if self.floor is not None else 0.0, node[1])


class ParkPlan:
    def __init__(self):
        self.lv = FlatLevel()
        self.tried = {}


def park_air(bot, plan):
    """An air: one grab (a d-pad tap and Square, held), let go before the landing; no spins, no flips (the park's
    short airs landed unfinished and lost their lines)."""
    taps = bot.rng.choice(ai.GRABS)
    start = bot.s.tick
    for _ in range(3):
        if not bot.s.air or ai.air_left(bot, plan, bot.s) < 0.5:
            break
        yield core.NOTHING
    if bot.s.air and ai.air_left(bot, plan, bot.s) >= 0.5:
        yield from wait(bot, 2, pad(*taps))
        yield from wait(bot, 2)
        bot.tally('grab')
        while bot.s.air and not bot.s.down and ai.air_left(bot, plan, bot.s) > 0.3 and bot.s.tick - start < ai.AIR_TOO_LONG:
            yield pad('SQUARE')
    while bot.s.air and not bot.s.down:
        if bot.s.tick - start > ai.AIR_TOO_LONG:
            yield from ai.out_of_the_air(bot)
            return
        yield core.NOTHING


def drive(bot, plan, target, near=3.0, limit=600, buttons=()):
    """To a point, steering; airs get a trick, a rail met on the way is ridden, a bail waits for the skater."""
    slow = 0
    for _ in range(limit):
        s = bot.s
        if s.mode == ai.PAUSED:
            yield from ai.close_boxes(bot)
            continue
        if ai.bailed(s):
            bot.tally('bail')
            yield from ai.recover(bot)
            return False
        if s.st == ai.GROUND and s.speed > 1.0 and isinstance(plan.lv, FlatLevel):
            plan.lv.floor = s.pos[1] if plan.lv.floor is None else max(plan.lv.floor, s.pos[1])
        if s.air:
            yield from park_air(bot, plan)
            yield from ai.on_the_ground(bot, 300)
            continue
        if s.grinding:
            bot.tally('grind on the way')
            yield from ai.ride_rail(bot, plan)
            continue
        if ai.flat_dist(s.pos, target) < near:
            return True
        slow = slow + 1 if s.speed < 1.5 else 0
        if slow > 90:
            bot.tally('stuck')
            yield from ai.unstick(bot)
            slow = 0
            continue
        yield ai.steer(s, target, *buttons)
    return False


def rail(bot, plan, a, b, what):
    """A rail from cell point a to b: a run-up along its line, the ollie and grind (ai.onto_rail), ridden."""
    on, end = W(*a), W(*b)
    d = tuple(q - p for p, q in zip(on, end))
    size = math.hypot(d[0], d[2])
    d = (d[0] / size, 0.0, d[2] / size)
    start = (on[0] - d[0] * 14.0, 0.0, on[2] - d[2] * 14.0)
    ok = yield from drive(bot, plan, start, near=3.5)
    if not ok:
        return False
    yield from drive(bot, plan, (on[0] - d[0] * 8.0, 0.0, on[2] - d[2] * 8.0), near=2.5)
    s = bot.s
    rx, rz = s.pos[0] - on[0], s.pos[2] - on[2]
    say(bot, '  %s: run-up at along %.1f across %.1f speed %.1f heading off %.2f' % (
        what, rx * d[0] + rz * d[2], rz * d[0] - rx * d[2], s.speed, abs(core.wrap(math.atan2(d[2], d[0]) - ai.heading(s)))))
    got = yield from ai.onto_rail(bot, plan, dict(on=on, dir=d))
    s = bot.s
    rx, rz = s.pos[0] - on[0], s.pos[2] - on[2]
    say(bot, '  %s: %s at along %.1f across %.1f y %.2f st %s' % (what, 'ON' if got else 'off', rx * d[0] + rz * d[2],
        rz * d[0] - rx * d[2], s.pos[1], ai.STATES[s.st] if 0 <= s.st < 18 else s.st))
    bot.tally('%s: %s' % (what, 'grind' if got else 'missed'))
    if got:
        yield from ai.ride_rail(bot, plan)
    return got


def ramp_line(bot, plan, a, b, what, passes=1):
    """A line through ramps: to cell a, then on to cell b (and back, PASSES times), airs tricked."""
    for k in range(passes):
        ok = yield from drive(bot, plan, W(*a), near=3.0)
        ok = ok and (yield from drive(bot, plan, W(*b), near=2.0, limit=400))
        bot.tally('%s %s' % (what, 'through' if ok else 'short'))
        a, b = b, a


def wall_ride(bot, plan, a, b, side, what):
    """A wall from cell a to b (its line), come at from SIDE (a cell direction) at about 30 degrees: jump, Down and
    Cross to take it (ai.wall_line's way), held while it lasts."""
    wa, wb = W(*a), W(*b)
    d = D(b[0] - a[0], b[1] - a[1])
    n = D(*side)
    mid = ((wa[0] + wb[0]) / 2, 0.0, (wa[2] + wb[2]) / 2)
    start = (mid[0] - d[0] * 12.0 + n[0] * 7.0, 0.0, mid[2] - d[2] * 12.0 + n[2] * 7.0)
    ok = yield from drive(bot, plan, start, near=3.5)
    if not ok:
        return False
    for _ in range(150):
        s = bot.s
        if ai.bailed(s):
            return False
        off = abs((mid[0] - s.pos[0]) * n[0] + (mid[2] - s.pos[2]) * n[2])
        if off < 3.5:
            break
        yield ai.steer(s, mid)
    yield from wait(bot, 3, pad('UP', 'CROSS'))
    for k in range(40):
        s = bot.s
        if s.st == ai.WALL_RIDE:
            break
        off = abs((mid[0] - s.pos[0]) * n[0] + (mid[2] - s.pos[2]) * n[2])
        yield pad('CROSS', 'DOWN') if off < 2.6 else ai.steer(s, mid)
    got = bot.s.st == ai.WALL_RIDE
    bot.tally('%s: %s' % (what, 'wall ride' if got else 'missed'))
    for _ in range(60):
        if bot.s.st != ai.WALL_RIDE:
            break
        yield core.NOTHING
    yield from ai.on_the_ground(bot, 300)
    return got


def rail_drill(bot, plan, ticks):
    """(RIDE=rails) The rails alone, again and again: the plaza's, the lawn's, the fun box."""
    end = bot.s.tick + ticks
    while bot.s.tick < end and bot.s.mode in (ai.PLAY, ai.PAUSED):
        yield from rail(bot, plan, (7, 9.6), (7, 11.4), 'plaza rail')
        yield from rail(bot, plan, (6.4, 13), (1.6, 13), 'lawn rail')
        yield from rail(bot, plan, (9.55, 9.4), (9.55, 5.6), 'plaza fun box')


def circuit(bot, plan, ticks):
    """The six zones in a ring, again and again: the tower's roll-ins and kickers, the plaza's fun box and rail, the
    hangar's mini ramp and wall, the yard's jump line, the lawn's long rail, the pools' bowl."""
    end = bot.s.tick + ticks
    lap = 0
    while bot.s.tick < end and bot.s.mode in (ai.PLAY, ai.PAUSED):
        lap += 1
        say(bot, 'lap %d at %s score %d' % (lap, [round(v, 1) for v in bot.s.pos], bot.s.score))
        steps = [
            lambda: ramp_line(bot, plan, (0.5, 7.5), (8, 7.5), 'tower drop'),
            lambda: rail(bot, plan, (7, 9.6), (7, 11.4), 'plaza rail'),
            lambda: rail(bot, plan, (6.4, 13), (1.6, 13), 'lawn rail'),
            lambda: ramp_line(bot, plan, (3.5, 4.5), (3.5, 0.6), 'pool', passes=2),
            lambda: rail(bot, plan, (9.55, 9.4), (9.55, 5.6), 'plaza fun box'),
            lambda: ramp_line(bot, plan, (13.5, 2.5), (13.5, 4.6), 'hangar mini ramp', passes=3),
            lambda: wall_ride(bot, plan, (10.6, 6), (14.4, 6), (0, -1), 'hangar wall'),
            lambda: ramp_line(bot, plan, (7.5, 15), (15, 15), 'yard jump'),
            lambda: ramp_line(bot, plan, (0.5, 13), (0.5, 8.0), 'tower bank'),
        ]
        for step in steps:
            if bot.s.tick >= end:
                break
            t0, sc0 = bot.s.tick, bot.s.score
            yield from step()
            say(bot, '  step done in %d ticks at %s, score %d (+%d)' % (bot.s.tick - t0, [round(v, 1) for v in bot.s.pos], bot.s.score, bot.s.score - sc0))


AI_RISE = 1.8             # the highest rail over its ground the ollie onto a rail (ai.onto_rail) reaches


def wall_world(bot, plan, f, e):
    """A wall from the map (parkmap's wall entry): in at about 30 degrees along its side, a jump, Down and Cross to
    take it (ai.wall_line's way), held while it lasts."""
    on, w, side = e['on'], e['dir'], e['side']
    for _ in range(200):
        s = bot.s
        if ai.bailed(s):
            return False
        off = abs((s.pos[0] - on[0]) * side[0] + (s.pos[2] - on[2]) * side[1])
        if off < 3.5:
            break
        yield ai.steer(s, (on[0] + w[0] * 4.0, on[1], on[2] + w[2] * 4.0))
    yield from wait(bot, 3, pad('UP', 'CROSS'))
    for k in range(40):
        s = bot.s
        if s.st == ai.WALL_RIDE:
            break
        off = abs((s.pos[0] - on[0]) * side[0] + (s.pos[2] - on[2]) * side[1])
        yield pad('CROSS', 'DOWN') if off < 2.6 else ai.steer(s, (on[0] + w[0] * 4.0, on[1], on[2] + w[2] * 4.0))
    got = bot.s.st == ai.WALL_RIDE
    for _ in range(60):
        if bot.s.st != ai.WALL_RIDE:
            break
        yield core.NOTHING
    yield from ai.on_the_ground(bot, 300)
    return got


def tour(bot, plan, ticks, reached):
    """(the default) The park's features from its own map (parkmap.py, the save's pieces placed as the game places
    them): the nearest not yet ridden, along a path over the map's cells to its run-up; rails ground, walls ridden,
    ramps ridden up into an air. A feature counts when the game's state reaches it (parkmap.Reached)."""
    lv = plan.lv
    t = parkmap.Tour(lv, kinds=('rail', 'wall', 'ramp'), least=5.0, rise=AI_RISE)
    end = bot.s.tick + ticks
    tries = {}
    while bot.s.tick < end and bot.s.mode in (ai.PLAY, ai.PAUSED):
        f = t.next(bot.s.pos)
        if f is None:
            yield from circuit(bot, plan, end - bot.s.tick)
            return
        e = f['entries'][tries.get(f['id'], 0) % len(f['entries'])]
        tries[f['id']] = tries.get(f['id'], 0) + 1
        what = '%s %s' % (f['id'], f['name'])
        t0, sc0, hits = bot.s.tick, bot.s.score, reached.hits[f['id']]
        path = lv.path(bot.s.pos, e['start'])
        yield from ai.follow(bot, (path or [])[1:] + [e['start']], near=4.0, limit=900)
        if f['kind'] == 'rail':
            on, d = e['on'], e['dir']
            yield from drive(bot, plan, (on[0] - d[0] * 8.0, 0.0, on[2] - d[2] * 8.0), near=2.5, limit=300)
            got = yield from ai.onto_rail(bot, plan, dict(on=on, dir=d))
            if got:
                yield from ai.ride_rail(bot, plan)
        elif f['kind'] == 'wall':
            yield from wall_world(bot, plan, f, e)
        else:
            yield from drive(bot, plan, e['on'], near=3.0, limit=300)
            yield from drive(bot, plan, e['through'], near=2.0, limit=300)
        yield from ai.on_the_ground(bot, 300)
        got = reached.hits[f['id']] > hits
        t.result(f, got)
        bot.tally('%s: %s' % (f['kind'], 'reached' if got else 'missed'))
        say(bot, '  %s: %s in %d ticks at %s, score %d (+%d)' % (what, 'reached' if got else 'missed', bot.s.tick - t0,
                                                                 [round(v, 1) for v in bot.s.pos], bot.s.score, bot.s.score - sc0))


def to_park(bot):
    """Main menu > FREE SKATE (left end, RIGHT once), the first skater, level select entry 7 (the Editor Level); the
    native park list: the first park."""
    yield from ai.boot(bot)
    for _ in range(6):
        yield from tap(bot, 'LEFT', hold=6, gap=30)
    yield from tap(bot, 'RIGHT', hold=6, gap=30)
    shot(bot, 'ride-menu')
    p0 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=60)
    p1 = yield from new_panel(bot, p0)
    yield from tap(bot, 'CROSS', gap=60)
    p2 = yield from new_panel(bot, p1)
    yield from wait(bot, 30)
    if bot.s.cursor == ai.NAME_ENTRY_END:
        yield from tap(bot, 'CROSS', gap=60)
        p2 = yield from new_panel(bot, p2)
    shot(bot, 'ride-levels')
    yield from set_cursor(bot, 7)
    say(bot, 'level select cursor', bot.s.cursor)
    yield from tap(bot, 'CROSS', gap=60)
    yield from wait(bot, 300)
    shot(bot, 'ride-load-1')
    say(bot, 'load: mode %d panel %#x cursor %d' % (bot.s.mode, bot.s.panel, bot.s.cursor))
    for k in range(6):
        if bot.s.mode == ai.PLAY:
            break
        yield from tap(bot, 'CROSS', hold=6, gap=150)
        shot(bot, 'ride-load-%d' % (k + 2))
        say(bot, 'load: mode %d panel %#x cursor %d' % (bot.s.mode, bot.s.panel, bot.s.cursor))
    yield from ai.wait_placed(bot)
    yield from wait(bot, 60)
    yield from ai.close_boxes(bot)
    shot(bot, 'ride-placed')
    say(bot, 'placed:', bot.s.placed, 'level', bot.s.level, 'pos', [round(v, 2) for v in bot.s.pos],
        'forward', [round(v, 2) for v in bot.s.forward])
    return bot.s.placed


def park_numbers(name):
    """The playbot's numbers from a ride's samples: ai.analyse's, and the rail metres (distance in a grind), the
    wall rides, the zones visited."""
    m = ai.analyse(name)
    rail_m, last, zones = 0.0, None, {}
    for r in ai.samples(name):
        if r['m'] != ai.PLAY or 'st' not in r or r['st'] >= len(ai.STATES) or abs(r['x'][1]) > 15000:
            last = None
            continue
        if last and r['st'] in ai.GRINDS and last['st'] in ai.GRINDS:
            d = math.dist(r['x'], last['x'])
            if d < 2:
                rail_m += d
        cx, cz = r['x'][0] / CELL_M + 7.5, -r['x'][2] / CELL_M + 7.5
        zone = zone_of(cx, cz)
        zones[zone] = zones.get(zone, 0) + 1
        last = r
    e = m['entries']
    return dict(ticks=m['ticks'], skate_s=round(m['skating'] / 59.94), distance_m=round(m['dist']), top_speed=round(m['top'], 1),
                grinds=sum(e.get(k, 0) for k in ('Grind', 'HandGrind', 'DoubleGrind')), rail_m=round(rail_m, 1),
                wall_rides=e.get('WallRide', 0), jumps=sum(e.get(k, 0) for k in ai.AIR_NAMES), air_s=round(m['air'] / 59.94, 1),
                landed=m['lands'], bails=m['bailland'] + m['crashes'], lines=m['lines'], tricks=m['tricks'], score=m['score'],
                best_combo=m['combo'], zones={k: round(v / 59.94) for k, v in sorted(zones.items())}, states=e)


ZONES = (('tower', 0, 4, 5, 11), ('pools', 0, 9, 0, 4), ('hangar', 10, 16, 0, 7), ('yard', 10, 16, 9, 16),
         ('lawn', 0, 9, 11, 16), ('plaza', 5, 10, 4, 11))


def zone_of(cx, cz):
    for name, x0, x1, z0, z1 in ZONES:
        if x0 <= cx < x1 and z0 <= cz < z1:
            return name
    return 'between'


def ride(name, parks, ticks, seed=1):
    import shutil
    saves = os.path.join(ai.SCRATCH, 'runs', name + '-saves')
    shutil.rmtree(saves, ignore_errors=True)
    os.makedirs(os.path.join(saves, 'Parks'))
    for f in sorted(os.listdir(parks)):
        if f.endswith('.sav'):
            shutil.copy(os.path.join(parks, f), os.path.join(saves, 'Parks', f))
    plan = ParkPlan()
    log = ai.Log(os.path.join(ai.SCRATCH, 'measure', name + '.samples.gz'))
    save = os.path.join(parks, sorted(f for f in os.listdir(parks) if f.endswith('.sav'))[0])
    m = parkmap.zge3_park_map('ai', save, name)
    parkmap.save(m, os.path.join(ai.SCRATCH, 'measure', name + '.parkmap'))
    lv = parkmap.Plan(m, run_up=14.0, clear=1.2)
    reached = parkmap.Reached(lv)
    print('map: %d cells, features %s' % (len(m['nodes']), dict(__import__('collections').Counter(f['kind'] for f in m['features']))), flush=True)

    memory, arena = [], []

    def behaviour(bot):
        ok = yield from to_park(bot)
        if not ok:
            return
        if os.environ.get('PARK_MEMORY'):
            arena.append(memuse.Arena(bot.run, 'ai'))
            memory.append(dict(tick=bot.s.tick, what='placed', **arena[0].sample()))
            say(bot, 'memory: %s' % memory[-1])
        if os.environ.get('RIDE') == 'rails':
            yield from rail_drill(bot, plan, ticks)
        elif os.environ.get('RIDE') == 'circuit':
            yield from circuit(bot, plan, ticks)
        else:
            plan.lv = lv
            yield from tour(bot, plan, ticks, reached)
        say(bot, 'ride over: score', bot.s.score)
        shot(bot, 'ride-end')

    def on_tick(s, p):
        log(s, p)
        if arena and memory and s.tick - memory[-1]['tick'] >= 3000:
            memory.append(dict(tick=s.tick, what='ride', **arena[0].sample()))
        if s.mode == ai.PLAY and s.placed:
            reached.tick(s.pos, 'grind' if s.grinding else 'wall' if s.st == ai.WALL_RIDE else 'air' if s.air else None)

    e = {'PS2X_HEADLESS': '1', 'PS2X_DEV_UNLOCK_ALL': '1', 'PS2X_SAVES': None, 'PS2X_SAVES_DIR': saves}
    with core.Run(ai.config(name, e, secs=max(900, ticks // 10), gs=SHOTS)) as run:
        bot = ai.Player(run, behaviour, seed)
        try:
            core.play(run, ai.read, bot, ticks + 12000, on_tick=on_tick)
        finally:
            log.close()
        print('notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:30], flush=True)
    out = park_numbers(name)
    out['notes'] = bot.notes
    if memory:
        log_path = os.path.join(ai.SCRATCH, 'runs', name, 'run.log')
        out['memory'] = dict(arena=memory, gsmem=memuse.gsmem(log_path), heap=memuse.heap(log_path))
        print('memory', json.dumps(out['memory']))
    out.update(reached.summary())
    out['track'] = reached.track
    core.save_json(os.path.join(ai.SCRATCH, 'measure', name + '.report.json'), out)
    print(json.dumps({k: v for k, v in out.items() if k not in ('states', 'notes', 'track', 'reached')}))
    print('reached', json.dumps(out['reached']))


def run_session(name, behaviour, ticks, gs=False, env=None):
    e = {'PS2X_HEADLESS': '1', 'PS2X_DEV_UNLOCK_ALL': '1'}
    e.update(env or {})
    with core.Run(ai.config(name, e, secs=max(600, ticks // 15), gs=gs or SHOTS)) as run:
        bot = ai.Player(run, behaviour, 1)
        core.play(run, ai.read, bot, ticks)
        return run


def catalog(name):
    out = {}

    def behaviour(bot):
        ok = yield from enter(bot)
        if not ok:
            return
        ed = Editor(bot.run)
        out['pieces'] = ed.catalog()
        # the cursor's d-pad: one tap each way, the cell read back
        moves = {}
        for b in ('RIGHT', 'LEFT', 'UP', 'DOWN'):
            c0 = ed.cursor()
            yield from tap(bot, b, hold=6, gap=30)
            moves[b] = [a - b0 for a, b0 in zip(ed.cursor(), c0)]
        out['dpad'] = moves
        out['start_cursor'] = ed.cursor()
        groups = []
        for g in range(16):
            groups.append(dict(group=g, tool=ed.tool_class(), piece=ed.piece()))
            pieces = []
            if ed.piece():
                for _ in range(20):
                    pieces.append(ed.piece()[0])
                    yield from tap(bot, 'R2', hold=6, gap=30)
                    if ed.piece()[0] == pieces[0]:
                        break
            groups[-1]['r2'] = pieces
            yield from tap(bot, 'R1', hold=6, gap=50)
        out['groups'] = groups

    run_session(name, behaviour, 40000)
    path = os.path.join(ai.SCRATCH, 'measure', name + '.catalog.json')
    core.save_json(path, out)
    print('catalog:', len(out.get('pieces', [])), 'pieces; dpad', out.get('dpad'), '->', path)


def probe(name):
    """Orientation and terrain: a 15' Quarter Pipe at each of four turns, a block raised, pictures."""
    log = []

    def behaviour(bot):
        ok = yield from enter(bot, int(os.environ.get('THEME', '1')))
        if not ok:
            return
        ed = Editor(bot.run)
        say(bot, 'theme index', bot.run.u32(ed.core + CORE_THEME_INDEX), 'cursor', ed.cursor(), 'cost', cost(bot))
        for t in range(4):
            yield from place(bot, 4, "15' Quarter Pipe", 2 + 3 * t, 2, turns=t, log=log)
        yield from terrain(bot, 2, 8, 3, 9, 15, log=log)
        yield from place(bot, 11, "15' Roll-In", 4, 8, turns=0, log=log)
        yield from place(bot, 1, "10' Ramp", 8, 8, turns=0, log=log)
        yield from place(bot, 10, "Centered Rail", 8, 10, turns=0, log=log)
        yield from place(bot, 10, "Centered Rail", 8, 11, turns=0, log=log)
        yield from overview(bot, name + '-overview')
        yield from wait(bot, 120)

    run_session(name, behaviour, 30000)
    core.save_json(os.path.join(ai.SCRATCH, 'measure', name + '.probe.json'), log)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'catalog':
        catalog(a[1])
    elif a and a[0] == 'ride':
        ride(a[1], a[2], int(a[3]), int(a[4]) if len(a) > 4 else 1)
    elif a and a[0] == 'build':
        build(a[1])
    elif a and a[0] == 'probe3':
        probe3(a[1])
    elif a and a[0] == 'probe2':
        probe2(a[1])
    elif a and a[0] == 'probe':
        probe(a[1])
    else:
        print(__doc__)
