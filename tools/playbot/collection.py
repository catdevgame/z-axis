#!/usr/bin/env python3
"""The collection app played: one process, its menu, a game, Exit to Collection, the next game.

    collection.py play NAME APP VISITS [TICKS]

APP: the collection app's executable (a copy of make-app.sh's, e.g. "Z-Axis Collection.app/Contents/MacOS/Z-Axis
Collection"). VISITS: the games in the order played, comma separated, from dm2, ai, bmx (the menu's order); each one is
booted from the collection's menu (ZX_MENU_SCRIPT: game:<id>, the menu's own step to that game's card, then Enter), played from its title
to a level and TICKS of riding there (the playbot players: ai.py's career_level, player_dm2 for DM2 and BMX XXX), then
left by the game's own Exit to Collection: DM2 and BMX XXX from the pause menu's last line, Aggressive Inline from its
main menu's Quit (the pause's EXIT first). The last visit ends in its level (the run stops there).

The game running is the app log's last `[app] ID: running` (ZX_APP_LOG_DIR); a game left is its `left for the menu`.
Menus in DM2 and BMX XXX are dm2routes' and bmxroutes' scripts played closed loop (their SYNCs waited on the game's
words); AI's are ai.py's. Numbers per visit: the game's own (DM2/BMX: score, tricks, distance via dm2.Meter; AI: score,
challenge points), and whether it left. Scratch: $PLAYCOLL (runs/NAME, NAME.json).
"""
import json
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
from core import pad, NOTHING  # noqa: E402

SCRATCH = os.environ.get('PLAYCOLL', os.path.join(zpaths.ZAXIS_SCRATCH, 'playcoll'))
ORDER = ('dm2', 'ai', 'bmx')          # the collection's games, as its menu shows them (release order); booted by id


class AppLog:
    """The app's log, read as it grows: which game runs, which left."""

    def __init__(self, folder):
        self.folder = folder
        self.at = 0
        self.lines = []
        self.running = None
        self.left = []

    def poll(self):
        path = next((os.path.join(self.folder, f) for f in sorted(os.listdir(self.folder))
                     if f.endswith('.log') and 'previous' not in f), None) if os.path.isdir(self.folder) else None
        if not path:
            return
        with open(path, 'rb') as f:
            f.seek(self.at)
            data = f.read()
        cut = data.rfind(b'\n') + 1
        self.at += cut
        for line in data[:cut].decode('utf-8', 'replace').splitlines():
            m = re.search(r'\[app\] (\w+): running', line)
            if m:
                self.running = m.group(1)
            m = re.search(r'\[app\] (\w+): left for the menu', line)
            if m:
                self.left.append(m.group(1))
                self.running = None
            if 'Exit to Collection' in line or 'quit to desktop' in line or '[app]' in line:
                self.lines.append(line)


def script_items(script):
    """A dm2routes/bmxroutes Script as (time, kind, value, hold): kind 'press' or 'sync'."""
    out = []
    for item in script.items:
        t, rest = item.split('v:', 1)
        if rest.startswith('SYNC=') or rest.startswith('WAIT='):
            out.append((int(t), 'sync', rest.split('=', 1)[1], 0))
        else:
            button, hold = rest.split(':')
            out.append((int(t), 'press', button, int(hold.rstrip('v'))))
    return sorted(out, key=lambda x: x[0])


def holds(run, cond, counters=()):
    """A SYNC's condition: ADDR==V (a counter in `counters`, the level's frame, may have gone past V between reads)."""
    a, v = cond.split('==')
    word = int.from_bytes(run.mem(int(a, 16), 4), 'little')
    return word == int(v, 0) or (int(a, 16) in counters and word > int(v, 0))


def play_script(v, script, boot_mode=None, counters=()):
    """The script's presses on its own clock from the game's boot (script time), its SYNCs waited for; while a SYNC on
    the front end's mode waits long (the title), START now and then."""
    items = script_items(script)
    offset = 0
    i = 0
    while i < len(items):
        t, kind, value, hold = items[i]
        st = v.st
        now = st.script - offset
        if now < t:
            yield NOTHING
            continue
        if kind == 'sync':
            waited = 0
            while not holds(v.run, value, counters):
                waited += 1
                if waited % 1200 == 0:
                    v.note('waiting %d for %s (script %d)' % (waited, value, v.st.script))
                yield pad('START') if boot_mode and value == boot_mode and waited > 900 and waited % 240 < 6 else NOTHING
                if waited > 20000:
                    v.note('sync never held: %s' % value)
                    return False
            offset = v.st.script - t
            v.note('sync %s held at script %d (tick %d)' % (value, v.st.script, v.st.tick))
            i += 1
            continue
        for _ in range(hold):
            yield pad(value)
        i += 1
    return True


class Visit:
    def __init__(self, run, log, game):
        self.run, self.log, self.game = run, log, game
        self.st = None
        self.notes = []

    def note(self, text):
        self.notes.append(text)
        print('[%s] %s' % (self.game, text), flush=True)


def leave_check(v, limit):
    for _ in range(limit):
        v.log.poll()
        if v.game in v.log.left or v.log.running != v.game:
            return True
        yield NOTHING
    return False


def ride_bmx_like(v, mod, level_name, seed, ticks):
    import dm2
    import dm2map
    import player_dm2
    # the level pickles: BMX XXX's in bmxmap's folder, DM2's in dm2map's own (one process plays both)
    dm2map.SCRATCH = os.environ.get('BMXCORPUS' if v.game == 'bmx' else 'PLAYDM2', dm2map.SCRATCH)
    lv = dm2map.Level(level_name)
    plog = player_dm2.Log(os.path.join(v.run.dir, '%s-play.log' % v.game))
    pl = mod.player_of(lv, seed, plog) if hasattr(mod, 'player_of') else player_dm2.Player(lv, seed, plog)
    meter = dm2.Meter()
    t0 = None
    while True:
        st = v.st
        meter.tick(st)
        if not st.riding:
            yield pad('CROSS') if st.script % 60 < 6 else NOTHING
            continue
        if st.pos[0] == 0.0 and st.pos[2] == 0.0:
            yield pad('CROSS') if st.script % 40 < 6 else NOTHING
            continue
        if t0 is None:
            t0 = st.script
        t = st.script - t0
        if t > ticks:
            break
        if t < 120:
            yield pad('CROSS') if 40 <= t < 46 else NOTHING
            continue
        yield pl(st)
    v.stats = meter.s.json()
    v.note('rode %d ticks: %s' % (ticks, {k: v.stats.get(k) for k in ('score', 'distance', 'jumps', 'c_tricks', 'c_grinds', 'bails')}))


def pause_exit(v, tries=3):
    """The pause menu, UP from its first line (it wraps to the last, EXIT TO COLLECTION), CROSS; closed on the log."""
    for k in range(tries):
        for _ in range(30):
            yield NOTHING
        for p in [pad('START')] * 6 + [NOTHING] * 60 + [pad('UP')] * 6 + [NOTHING] * 30 + [pad('CROSS')] * 6:
            yield p
        if (yield from leave_check(v, 600)):
            v.note('left by the pause menu (try %d)' % k)
            return True
        v.note('pause menu try %d: not left' % k)
        for p in [pad('TRIANGLE')] * 6 + [NOTHING] * 60 + [pad('START')] * 6 + [NOTHING] * 60:
            yield p
    return False


def photo_step(v, line, close):
    """(COLL_PHOTO=1) The pause menu's PHOTO MODE (`line` lines below CONTINUE, as the game's log says), the camera
    moved, Cross (the app's Screenshot: a picture in ZX_SCREENSHOT_DIR), Circle; back up to CONTINUE and `close`."""
    def taps(b, n, gap=20):
        for _ in range(n):
            for p in [pad(b)] * 6 + [NOTHING] * gap:
                yield p
    for p in [NOTHING] * 30 + [pad('START')] * 6 + [NOTHING] * 60:
        yield p
    yield from taps('DOWN', line)
    yield from taps('CROSS', 1, 60)
    for p in [pad(ly=-1.0)] * 90 + [pad(rx=1.0)] * 60 + [NOTHING] * 30:
        yield p
    yield from taps('CROSS', 1, 120)
    yield from taps('CIRCLE', 1, 90)
    yield from taps('UP', line)
    yield from taps(close, 1, 90)
    v.log.poll()
    saved = [l for l in v.log.lines if 'screenshot' in l and ('saved' in l or 'failed' in l)]
    v.note('photo mode: %s' % (saved[-1] if saved else 'no screenshot line'))


def visit_dm2(v, ticks, seed, reentry):
    import importlib
    import dm2
    import dm2routes
    importlib.reload(dm2routes)       # (bmxroutes writes BMX's words into dm2routes' globals: MODE, LEVEL_FRAME, ...)
    v.read = dm2.read
    s = dm2.menu_script('freeride', 0)
    ok = yield from play_script(v, s, boot_mode='0x%X==1' % dm2routes.MODE, counters=(dm2routes.LEVEL_FRAME,))
    v.note('to Woodward free ride: %s' % ok)
    yield from ride_bmx_like(v, dm2, 'WOODWARD', seed, ticks)
    if os.environ.get('COLL_PHOTO') == '1':
        yield from photo_step(v, 6, 'CROSS')
    return (yield from pause_exit(v))


def visit_bmx(v, ticks, seed, reentry):
    import importlib
    import bmx
    import bmxroutes
    importlib.reload(bmxroutes)       # (its words into dm2routes again, after a DM2 visit's reload)
    v.read = bmx.read
    s = bmx.menu_script(0, 0)
    ok = yield from play_script(v, s, boot_mode='0x%X==1' % bmxroutes.MODE, counters=(bmxroutes.LEVEL_FRAME,))
    v.note('to the Bronx (Hardcore Tour): %s' % ok)
    yield from ride_bmx_like(v, bmx, 'CHL_MULY', seed, ticks)
    if os.environ.get('COLL_PHOTO') == '1':
        yield from photo_step(v, 7, 'START')
    return (yield from pause_exit(v))


def visit_ai(v, ticks, seed, reentry):
    import ai
    import nav
    v.read = ai.read
    import importlib
    importlib.reload(nav)             # (dm2map.Level points nav's globals at the BMX/DM2 pickles and links)
    box = {}

    def behaviour(bot):
        yield from ai.boot(bot)
        yield from ai.career(bot, 1)
        v.note('placed: level %d at %s' % (bot.s.level, [round(x, 1) for x in bot.s.pos]))
        yield from ai.career_level(bot, ai.Plan(nav.LEVELS[1]), ticks)
        yield from ai.on_the_ground(bot)
        v.stats = dict(score=bot.s.score, points=ai.levels_points(bot.s))
        v.note('rode: %s' % v.stats)
        if box.get('last'):
            return
        # the pause's EXIT (the menu's last line, a YES/NO box), then the main menu's Quit (Back, its box)
        yield from ai.close_boxes(bot)
        yield from ai.tap(bot, 'START', gap=40)
        yield from ai.bottom_of_menu(bot)
        v.note('pause: cursor %d (EXIT), mode %d' % (bot.s.cursor, bot.s.mode))
        yield from ai.tap(bot, 'CROSS', gap=60)
        for k in range(6):
            if bot.s.mode == ai.FRONT_END:
                break
            v.note('after EXIT: mode %d panel %#x cursor %d' % (bot.s.mode, bot.s.panel, bot.s.cursor))
            yield from ai.tap(bot, 'UP' if k % 2 == 0 else 'CROSS', gap=60)
        ok = yield from ai.until(bot, lambda s: s.mode == ai.FRONT_END and s.panel != 0, 3600)
        v.note('front end: %s, panel %#x' % (ok, bot.s.panel))
        yield from ai.wait(bot, 240)
        # Back (TRIANGLE) asks to quit: its box opens on its first line (cursor 0, No); the other line, CROSS
        for k, other in enumerate(('DOWN', 'UP', 'DOWN')):
            yield from ai.tap(bot, 'TRIANGLE', gap=120)
            v.note('quit asked: mode %d panel %#x cursor %d' % (bot.s.mode, bot.s.panel, bot.s.cursor))
            yield from ai.tap(bot, other, gap=60)
            v.note('quit box: %s, cursor %d' % (other, bot.s.cursor))
            yield from ai.tap(bot, 'CROSS', gap=240)
            v.log.poll()
            if v.log.running != 'ai':
                return

    bot = ai.Player(v.run, behaviour, seed)
    box['last'] = v.last
    while True:
        p = bot(v.st)
        if p is None:
            break
        yield p
        v.log.poll()
        if v.log.running != 'ai':
            v.note('left by the main menu')
            return True
    return False


VISITS = {'dm2': visit_dm2, 'bmx': visit_bmx, 'ai': visit_ai}


def _reader(module):
    def get():
        return __import__(module).read
    return get


READERS = {'dm2': _reader('dm2'), 'bmx': _reader('bmx'), 'ai': _reader('ai')}


def menu_script(visits):
    """Each visit's game by its id (game:<id>: the menu steps Left or Right to its card, wherever it shows it), Enter;
    a wait first."""
    steps = []
    for g in visits:
        assert g in ORDER, g
        steps += ['wait'] * 3 + ['game:' + g, 'Enter']
    return ','.join(steps)


def play(name, app, visits, ticks=3000, seed=1):
    os.makedirs(os.path.join(SCRATCH, 'runs'), exist_ok=True)
    env = {'ZX_MENU_SCRIPT': menu_script(visits), 'ZX_EE_SCALE': '4096', 'ZX_HEADLESS': '1',
           'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy', 'ZX_MC_FD_BASE0': '1'}
    cfg = core.RunConfig(name=name, runner=app, elf='', root=os.path.join(SCRATCH, 'runs'), env=env,
                         max_secs=600 + ticks * len(visits) // 10, script='99999999v:SELECT:1v',
                         gs=os.environ.get('COLL_GS') == '1')     # (COLL_GS=1: drawn, for the pictures photo mode saves)
    results = []
    with core.Run(cfg) as run:
        log = AppLog(os.path.join(run.dir, 'app'))
        state = {}

        def driver():
            for k, g in enumerate(visits):
                while True:
                    log.poll()
                    if log.running == g:
                        break
                    yield NOTHING
                v = Visit(run, log, g)
                v.last = k == len(visits) - 1
                v.stats = {}
                state['v'] = v
                v.note('visit %d: running' % k)
                v.read = READERS[g]()
                yield NOTHING                 # (the next read is this game's)
                gen = VISITS[g](v, ticks, seed + k, g in visits[:k])
                left = None
                try:
                    p = next(gen)
                    while True:
                        yield p
                        p = gen.send(None)
                except StopIteration as e:
                    left = e.value
                results.append(dict(game=g, left=left, stats=v.stats, notes=v.notes))
                if v.last:
                    return

        gen = driver()

        def read(run_):
            v = state.get('v')
            return v.read(run_) if v and hasattr(v, 'read') else core_state(run_)

        def policy(st):
            v = state.get('v')
            if v:
                v.st = st
            try:
                return next(gen)
            except StopIteration:
                return None

        core.play(run, read, policy, max_ticks=(ticks + 30000) * len(visits))
        log.poll()
        app_lines = log.lines
    out = dict(name=name, visits=list(visits), results=results, app=app_lines[-40:])
    core.save_json(os.path.join(SCRATCH, name + '.json'), out)
    for r in results:
        print(r['game'], 'left' if r['left'] else 'NOT LEFT', r['stats'])


class Bare:
    def __init__(self, run):
        self.tick, self.script = run.tick, run.script_time


def core_state(run):
    return Bare(run)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'play':
        play(a[1], a[2], a[3].split(','), int(a[4]) if len(a) > 4 else 3000)
    else:
        raise SystemExit(__doc__)
