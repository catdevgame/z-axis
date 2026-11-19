#!/usr/bin/env python3
"""BMX XXX: the custom-levels test park (docs/CUSTOM-LEVELS.md section 3, step 0) built in its park editor (AI's
editor, copied: zge/editor and game/bmx/editor), saved as a native file, and ridden.

    bmxpark.py probe NAME                orientation pictures: pieces at each turn (as aipark.py probe3)
    bmxpark.py build NAME                the six-zone park on Mullaly (30 x 20 cells of 5 m), saved: <saves>/Parks/Park 000.sav
    bmxpark.py ride NAME PARKS TICKS [SEED]  the saved park (PARKS: its Parks folder) ridden from PARK EDITOR > SESSION;
                                         dm2.Meter's numbers in $BMXCORPUS/sessions/NAME/park.json

In by the app's main menu line (PS2X_BMX_PARK_EDITOR=1: PARK EDITOR third), as bmxedit.py's BMXEDIT_MENU=1. The editor
is aipark.py's: the same core and tool set (EditCore, EditToolSet, EditSelection), at BMX XXX's addresses (the bind
table's: AI's g_parkEditorCore and g_parkCost). SHOTS=1: pictures.
"""
import json
import math
import os
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import bmx  # noqa: E402
import aipark  # noqa: E402
import ai  # noqa: E402
import zpaths  # noqa: E402
from core import pad, NOTHING  # noqa: E402

SHOTS = aipark.SHOTS


def bmx_address(ai_address):
    """AI's data address in BMX XXX, from the bind table (GAME_BIND_DIR/bmx.tsv: kind, ai, ai_end, game, ...)."""
    path = os.path.join(zpaths.PATHS.get('GAME_BIND_DIR', os.path.join(zpaths.ZAXIS_SCRATCH, 'workdb', 'bind')), 'bmx.tsv')
    with open(path) as f:
        for line in f:
            w = line.split('\t')
            if len(w) > 3 and w[0] == 'data' and int(w[1], 16) == ai_address:
                return int(w[3], 16)
    raise KeyError('%#x' % ai_address)


EDIT_CORE = bmx_address(ai.SYM['g_parkEditorCore'])
PARK_COST = bmx_address(ai.SYM['g_parkCost'])
from bmxedit import GAME_MODE  # noqa: E402  (0 front end, 1 game, 2 park editor, 3 a park played)
# The roll: 0 Terrain, 1-12 the Object Tool's groups (OBJECT.OBJ's categories RAMPS .. FIXED RAILS, 0-11), 13 the Rail
# Tool, 14 Start, 15 Dynamics (the theme's ToolsList after the objects)
CATEGORY_OF_GROUP = {g: g - 1 for g in range(1, 13)}
RAIL_TOOL, START_TOOL = 13, 14


def use_bmx():
    """aipark's editor functions read BMX XXX's editor."""
    aipark.EDIT_CORE = EDIT_CORE
    aipark.G_PARK_COST = PARK_COST
    aipark.CATEGORY_OF_GROUP = CATEGORY_OF_GROUP


class Bot:
    def __init__(self, run, seed=1):
        self.run, self.s = run, None
        self.notes = {}
        import random
        self.rng = random.Random(seed)

    def tally(self, what):
        self.notes[what] = self.notes.get(what, 0) + 1

    def game_mode(self):
        return self.run.mem(GAME_MODE, 1)[0]


def wait(n, p=NOTHING):
    for _ in range(n):
        yield p


def tap(button, hold=6, gap=30):
    yield from wait(hold, pad(button))
    yield from wait(gap)


def editing(bot):
    return bot.game_mode() == 2 and bot.s.mode == 2 and not bot.s.rider


def enter(bot, theme=0):
    """The main menu's PARK EDITOR line (third), NEW PARK, the theme (RIGHT a theme), the editor."""
    ed = aipark.Editor(bot.run)
    for _ in range(3000):
        if bot.s.mode == 1:
            break
        yield NOTHING
    yield from wait(240)
    aipark.shot(bot, 'main-menu')
    yield from tap('DOWN', gap=40)
    yield from tap('DOWN', gap=40)
    yield from tap('CROSS', hold=8, gap=200)
    yield from tap('CROSS', hold=8, gap=200)       # NEW PARK
    aipark.shot(bot, 'themes')
    for _ in range(theme):
        yield from tap('RIGHT', gap=40)
    yield from tap('CROSS', hold=8, gap=300)
    for _ in range(4000):
        if editing(bot) and ed.cursor() is not None:
            break
        yield NOTHING
    yield from wait(300)
    aipark.shot(bot, 'editor')
    ok = editing(bot) and ed.cursor() is not None
    aipark.say(bot, 'editor:', 'yes' if ok else 'NO', 'game mode', bot.game_mode(), 'cursor', ed.cursor(), 'tool',
               ed.tool_class(), 'theme index', bot.run.u32(ed.core + aipark.CORE_THEME_INDEX) if ed.core else None)
    bot.pe_at = 0
    return ok


def session(name, program, ticks=60000, env=None):
    import dm2  # noqa: F401  (the dm2 tools, bmxroutes among them, on the path)
    import bmxroutes
    menu = bmxroutes.Script()
    e = dict(PS2X_EE_SCALE='4096', PS2X_BMX_PARK_EDITOR='1', PS2X_HEADLESS='1')
    e.update(env or {})
    cfg = bmx.run_config(name, menu.text(), env=e, max_secs=max(900, ticks // 12), gs=SHOTS)
    with core.Run(cfg) as run:
        bot = Bot(run)
        gen = program(bot)

        def policy(st):
            bot.s = st
            if st.script < menu.t + 30:
                return NOTHING
            try:
                return next(gen)
            except StopIteration:
                return None

        core.play(run, bmx.read, policy, max_ticks=ticks)
    return bot, run


def probe(name):
    """Orientation, close up (aipark.probe3's way): each piece at each turn on (15, 10), the cursor three cells along +x
    behind it, a picture, Square."""
    use_bmx()
    pieces = [(3, '4M QUARTER PIPE'), (2, 'KICKER FOUR'), (5, 'ROLL IN FOUR'), (8, 'HIP A')]
    log = []

    def program(bot):
        ok = yield from enter(bot)
        if not ok:
            return
        ed = aipark.Editor(bot.run)
        moves = {}
        for b in ('RIGHT', 'LEFT', 'UP', 'DOWN'):
            c0 = ed.cursor()
            yield from tap(b, gap=30)
            moves[b] = [a - b0 for a, b0 in zip(ed.cursor(), c0)]
        aipark.say(bot, 'd-pad', moves)
        groups = []
        for g in range(16):
            groups.append((g, ed.tool_class()[:1], ed.piece()))
            yield from tap('R1', gap=50)
        aipark.say(bot, 'roll', groups)
        bot.pe_at = 0
        for g, piece in pieces:
            for t in range(4):
                yield from aipark.place(bot, g, piece, 15, 10, turns=t, log=log)
                yield from aipark.goto(bot, 18, 10)
                yield from wait(40)
                aipark.shot(bot, '%s-%s-%d' % (name, piece.replace(' ', ''), t))
                yield from aipark.goto(bot, 15, 10)
                yield from tap('SQUARE', gap=40)

    session(name, program)
    print(json.dumps(log))


# ---- the park: the AI park's layout (aipark.LAYOUT) on Mullaly's 30 x 20 grid of 5 m cells, AI's cell (x, z) at
# (x + 7, z + 2). The d-pad, the camera and the pieces' turns are AI's (probe's pictures: a quarter pipe, kicker or
# roll-in faces -z, +x, +z, -x at turns 0-3). Heights are metres (MapHeightIncrement 1; the ground at 6 of 12): the
# deck goes up 4 for ROLL IN FOUR.
MULLALY = 0
OX, OZ = 7, 2
TERRAIN = [('tower', (OX + 0, OZ + 7, OX + 1, OZ + 8), 4)]
LAYOUT = [
    ('tower', 5, 'ROLL IN FOUR', OX + 2, OZ + 7, 1),
    ('tower', 5, 'ROLL IN FOUR', OX + 2, OZ + 8, 1),
    ('tower', 2, 'KICKER FOUR', OX + 5, OZ + 7, 3),
    ('tower', 2, 'KICKER FOUR', OX + 5, OZ + 8, 3),
    ('tower', 1, 'RAMP FOUR', OX + 0, OZ + 10, 2),
    ('tower', 1, 'RAMP FOUR', OX + 1, OZ + 10, 2),
    ('plaza', 8, 'FUNBOX A', OX + 10, OZ + 8, 0),
    ('plaza', 8, 'FUNBOX B', OX + 7, OZ + 4, 0),
    ('plaza', 6, 'QUAD STAIRS', OX + 9, OZ + 11, 1),
    ('pools', 9, 'VERT BOWL', OX + 3, OZ + 2, 0),
    ('pools', 9, 'ROLLOUT BOWL', OX + 7, OZ + 1, 0),
    ('hangar', 3, '4M QUARTER PIPE', OX + 13, OZ + 4, 0),
    ('hangar', 3, '4M QUARTER PIPE', OX + 14, OZ + 4, 0),
    ('hangar', 3, '4M QUARTER PIPE', OX + 13, OZ + 1, 2),
    ('hangar', 3, '4M QUARTER PIPE', OX + 14, OZ + 1, 2),
    ('hangar', 11, 'BILLBOARD', OX + 12, OZ + 6, 0),
    ('yard', 8, 'HIP A', OX + 13, OZ + 11, 0),
    ('yard', 8, 'HIP WALL A', OX + 12, OZ + 14, 0),
    ('yard', 10, 'DOUBLE STEP', OX + 13, OZ + 15, 1),
    ('lawn', 12, 'BOW RAIL', OX + 3, OZ + 15, 0),
    ('lawn', 8, 'FUNBOX C', OX + 6, OZ + 17, 0),       # (x1: refused at +5, +15)
]
# straight rails: the rail tool on bare ground (AI's turns: 0 along x, 1 along z)
RAILS = [('plaza', OX + 7, OZ + 10, 1), ('plaza', OX + 7, OZ + 11, 1)] + [('lawn', OX + x, OZ + 13, 0) for x in range(2, 7)]
START = (OX + 0, OZ + 7, 1)      # BMX XXX's start marker: turns 1 faces +x (AI's: 3; x1's 3 faced -x)


def rail_tool(bot, x, z, turns, zone, log):
    """The Rail Tool (roll 13) at (x, z): turned, X; placed if the park's cost moved."""
    yield from aipark.roll(bot, RAIL_TOOL)
    for _ in range(turns % 4):
        yield from tap('CIRCLE', gap=30)
    yield from aipark.goto(bot, x, z)
    c0 = aipark.cost(bot)
    yield from tap('CROSS', gap=60)
    c1 = aipark.cost(bot)
    aipark.say(bot, 'rail at %d,%d turns %d: %s cost %d' % (x, z, turns, 'placed' if c1 != c0 else 'NOT placed', c1))
    log.append(dict(zone=zone, rail=(x, z), turns=turns, placed=c1 != c0, cost=c1 - c0))
    for _ in range((4 - turns % 4) % 4):
        yield from tap('CIRCLE', gap=30)


def start_tool(bot, x, z, turns):
    yield from aipark.roll(bot, START_TOOL)
    aipark.say(bot, 'start tool: %s' % aipark.Editor(bot.run).tool_class()[:1])
    for _ in range(turns % 4):
        yield from tap('CIRCLE', gap=30)
    yield from aipark.goto(bot, x, z)
    yield from tap('CROSS', gap=60)


def pause_item(bot, k):
    yield from tap('START', gap=90)
    for _ in range(k):
        yield from tap('DOWN', hold=6, gap=25)
    yield from tap('CROSS', hold=10, gap=60)


def save(bot):
    """(bmxedit.py's save) SAVE: NEW, the description and author keyboards' END, Yes: <saves>/Parks/Park NNN.sav."""
    yield from pause_item(bot, 2)
    yield from wait(200)
    for k, gap in enumerate((300, 300, 300, 600)):
        aipark.shot(bot, 'save-%d' % k)
        yield from tap('CROSS', hold=10, gap=gap)
    yield from tap('UP', gap=40)
    aipark.shot(bot, 'save-4')
    yield from tap('CROSS', hold=10, gap=900)
    aipark.shot(bot, 'save-5')


def build(name):
    use_bmx()
    log = dict(terrain=[], pieces=[], rails=[])

    def program(bot):
        ok = yield from enter(bot, MULLALY)
        if not ok:
            return
        ed = aipark.Editor(bot.run)
        log['catalog'] = ed.catalog()
        log['world'] = {}
        for c in ((15, 10), (0, 0), (29, 19)):
            yield from aipark.goto(bot, *c)
            yield from wait(90)
            log['world']['%d,%d' % c] = ed.selection_position()
        aipark.say(bot, 'cells in the world:', log['world'])
        for zone, rect, feet in TERRAIN:
            yield from aipark.terrain(bot, *rect, feet, log=log['terrain'])
        for zone, g, piece, x, z, turns in LAYOUT:
            yield from aipark.place(bot, g, piece, x, z, turns=turns, log=log['pieces'])
            log['pieces'][-1]['zone'] = zone
        for zone, x, z, turns in RAILS:
            yield from rail_tool(bot, x, z, turns, zone, log['rails'])
        yield from start_tool(bot, *START)
        log['cost'] = aipark.cost(bot)
        yield from aipark.goto(bot, OX + 15, OZ + 8)
        yield from aipark.overview(bot, name + '-park')
        aipark.say(bot, 'built: %d of %d pieces, %d of %d rails, cost %d' % (
            sum(p['placed'] for p in log['pieces']), len(LAYOUT), sum(p['placed'] for p in log['rails']), len(RAILS), log['cost']))
        yield from save(bot)
        yield from wait(200)

    bot, run = session(name, program, ticks=70000)
    saves = []
    for root, _, files in os.walk(run.dir):
        for f in files:
            if f.endswith('.sav') or 'PARK' in f.upper():
                saves.append((os.path.relpath(os.path.join(root, f), run.dir), os.path.getsize(os.path.join(root, f))))
    log['saves'] = saves
    core.save_json(os.path.join(bmx.SCRATCH, 'sessions', name + '.build.json'), log)
    print('saves:', saves)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'build':
        build(a[1])
    elif a and a[0] == 'probe':
        probe(a[1])
    else:
        print(__doc__)
