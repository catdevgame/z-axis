#!/usr/bin/env python3
"""Photo mode and the Settings page from the pause menu, played in a level (DM2, BMX XXX), checked by the game's state.

    photo.py play GAME NAME [LEVEL]   GAME dm2|bmx: into the level (Free Ride / Hardcore Tour) by the game's menus,
                                      ride, START, PHOTO MODE (the camera moved, Cross saves a picture, Circle back),
                                      then SETTINGS (every section, a row stepped Right and Left, Back), CONTINUE, ride

With the app's settings (ZX_GAME_SETTINGS_MENUS=1: the pause menu's PHOTO MODE and SETTINGS lines) and GS on (the
picture is the point); ZX_SCREENSHOT_DIR the run's photos/. The pause menu's lines are counted from its end: SETTINGS
last, PHOTO MODE the one above (dave_mirra_settings.cpp, bmx_xxx_exit.cpp); the shots show each screen.

Its numbers: the rider's position and the level's frame count before, all through and after photo mode (the game
stopped while it is on: the same frame count), the photos saved (files, sizes), the rider riding again after.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
from core import pad, NOTHING  # noqa: E402


def game_of(name):
    import dm2
    import bmx
    return {'dm2': dm2, 'bmx': bmx}[name]


class Bot:
    def __init__(self, run, g, log):
        self.run, self.g, self.log = run, g, log
        self.st = None
        self.rows = []

    def tap(self, button, hold=6, gap=40):
        for _ in range(hold):
            yield pad(button)
        for _ in range(gap):
            yield NOTHING

    def wait(self, n, p=NOTHING):
        for _ in range(n):
            yield p

    def note(self, what):
        st = self.st
        row = dict(what=what, tick=st.tick, script=st.script, mode=st.mode, frame=st.frame,
                   pos=[round(v, 2) for v in st.pos])
        self.rows.append(row)
        self.log(json.dumps(row))

    def shot(self, what):
        self.log('shot ' + self.run.shot(what))

    def ride(self, n):
        for t in range(n):
            lx = 0.5 * ((t // 120) % 3 - 1)
            yield pad('UP', lx=lx)

    def behaviour(self, game):
        yield from self.wait(120)
        if game == 'bmx':
            yield from self.tap('CROSS', gap=120)      # the level's box: CONTINUE
            yield from self.tap('CROSS', gap=120)      # CHALLENGES: CONTINUE
        self.note('ride start')
        yield from self.ride(600)
        self.note('riding')
        yield from self.tap('START', gap=90)
        self.shot('pause')
        self.note('paused')
        yield from self.tap('UP', gap=30)              # (the menu wraps: its last line, SETTINGS)
        yield from self.tap('UP', gap=30)              # PHOTO MODE
        self.shot('pause-photo-line')
        yield from self.tap('CROSS', gap=60)
        self.note('photo mode')
        self.shot('photo-in')
        yield from self.wait(90, pad(ly=-1.0))          # move forward
        yield from self.wait(60, pad(rx=1.0))           # look right
        yield from self.wait(40, pad('R2'))             # up
        self.note('camera moved')
        self.shot('photo-moved')
        yield from self.tap('CROSS', gap=90)           # the picture saved
        self.note('photo saved')
        yield from self.tap('CIRCLE', gap=90)          # back
        self.note('photo left')
        self.shot('photo-out')
        yield from self.tap('DOWN', gap=30)            # SETTINGS (the cursor on PHOTO MODE)
        self.shot('pause-settings-line')
        yield from self.tap('CROSS', gap=120)
        self.shot('settings')
        for k in range(6):                             # each section: its first row Right, Left; Down; the next
            yield from self.tap('RIGHT', gap=24)
            yield from self.tap('LEFT', gap=24)
            yield from self.tap('DOWN', gap=16)
            self.shot('settings-%d' % k)
            yield from self.tap('R1', gap=30)
        self.note('settings walked')
        yield from self.tap('TRIANGLE', gap=120)       # the pause menu again
        self.shot('settings-out')
        if game == 'dm2':
            yield from self.tap('CROSS', gap=90)       # (back from the page the cursor is on CONTINUE; Start does not
                                                       # close DM2's pause menu)
        else:
            yield from self.tap('START', gap=90)
        self.note('continued')
        yield from self.ride(600)
        self.note('riding again')
        self.shot('riding-again')
        yield from self.wait(30)


def play(game, name, level=0):
    g = game_of(game)
    import player_dm2
    if game == 'dm2':
        menu = g.menu_script('freeride', level)
    else:
        menu = g.menu_script(level, 0)
    d = os.path.join(g.SCRATCH, 'sessions', name)
    os.makedirs(d, exist_ok=True)
    env = {'ZX_GAME_SETTINGS_MENUS': '1', 'ZX_EE_SCALE': '4096'}
    cfg = g.run_config(name, menu.text(), env=env, max_secs=int(os.environ.get('MAX_SECS', '900')), gs=True)
    photos = os.path.join(cfg.root, name, 'photos')
    cfg.env['ZX_SCREENSHOT_DIR'] = photos
    log = player_dm2.Log(os.path.join(d, 'play.log'))
    with core.Run(cfg) as run:
        bot = Bot(run, g, lambda text: log(bot.st, text))
        program = bot.behaviour(game)
        started = {}

        def policy(st):
            bot.st = st
            if st.script < menu.t + 30 or not st.frame:
                if st.script > menu.t + 3000:
                    return None
                return NOTHING
            started.setdefault('at', st.script)
            try:
                return next(program)
            except StopIteration:
                return None

        core.play(run, g.read, policy, max_ticks=menu.t + 20000)
    files = sorted((f, os.path.getsize(os.path.join(photos, f))) for f in os.listdir(photos)) if os.path.isdir(photos) else []
    by = {r['what']: r for r in bot.rows}
    out = dict(rows=bot.rows, photos=files)
    if 'photo mode' in by and 'photo left' in by:
        out['frozen'] = by['photo mode']['pos'] == by['photo left']['pos']
    if 'continued' in by and 'riding again' in by:
        out['rode_after'] = by['riding again']['pos'] != by['continued']['pos']
    core.save_json(os.path.join(d, 'photo.json'), out)
    print(name, json.dumps({k: v for k, v in out.items() if k != 'rows'}))
    for r in bot.rows:
        print(' ', r)


def play_ai(name, line_photo=None, line_settings=None):
    """Aggressive Inline: Movie Lot's career on a fresh card, the pause menu's lines walked (the cursor read at each),
    then (with the lines' numbers) PHOTO MODE and SETTINGS as for the other two; CONTINUE (the first line)."""
    import ai
    rows, cursors = [], []
    photos = os.path.join(ai.SCRATCH, 'runs', name, 'photos')
    env = {'ZX_GAME_SETTINGS_MENUS': '1', 'ZX_SCREENSHOT_DIR': photos}

    def note(bot, what):
        s = bot.s
        row = dict(what=what, tick=s.tick, mode=s.mode, frame=s.frame, pos=[round(v, 2) for v in s.pos])
        rows.append(row)
        print(' ', row, flush=True)

    def shot(bot, what):
        print('  shot', bot.run.shot(what), flush=True)

    def to_line(bot, k):
        yield from ai.set_cursor(bot, k, inc='DOWN', dec='UP')

    def behaviour(bot):
        yield from ai.boot(bot)
        yield from ai.career(bot, 1)
        for t in range(600):
            yield pad('UP', lx=0.5 * ((t // 120) % 3 - 1))
        note(bot, 'riding')
        yield from ai.tap(bot, 'START', gap=60)
        note(bot, 'paused')
        shot(bot, 'pause')
        last = None
        for _ in range(16):                         # the lines, top to bottom: the cursor at each
            cursors.append(bot.s.cursor)
            yield from ai.tap(bot, 'DOWN', gap=20)
            if bot.s.cursor == last:
                break
            last = bot.s.cursor
            if line_photo is None and bot.s.cursor >= 5:
                shot(bot, 'pause-line-%d' % bot.s.cursor)
                yield from ai.wait(bot, 10)
        if line_photo is None:
            yield from ai.wait(bot, 30)
            return
        yield from to_line(bot, line_photo)
        shot(bot, 'pause-photo-line')
        yield from ai.tap(bot, 'CROSS', gap=60)
        note(bot, 'photo mode')
        shot(bot, 'photo-in')
        yield from ai.wait(bot, 90, pad(ly=-1.0))
        yield from ai.wait(bot, 60, pad(rx=1.0))
        yield from ai.wait(bot, 40, pad('R2'))
        note(bot, 'camera moved')
        shot(bot, 'photo-moved')
        yield from ai.tap(bot, 'CROSS', gap=90)
        note(bot, 'photo saved')
        yield from ai.tap(bot, 'CIRCLE', gap=90)
        note(bot, 'photo left')
        shot(bot, 'photo-out')
        if line_settings is not None:
            yield from to_line(bot, line_settings)
            shot(bot, 'pause-settings-line')
            yield from ai.tap(bot, 'CROSS', gap=120)
            shot(bot, 'settings')
            for k in range(6):
                yield from ai.tap(bot, 'RIGHT', gap=24)
                yield from ai.tap(bot, 'LEFT', gap=24)
                yield from ai.tap(bot, 'DOWN', gap=16)
                shot(bot, 'settings-%d' % k)
                yield from ai.tap(bot, 'R1', gap=30)
            note(bot, 'settings walked')
            yield from ai.tap(bot, 'TRIANGLE', gap=120)
            shot(bot, 'settings-out')
        yield from to_line(bot, cursors[0] if cursors else 0)
        yield from ai.tap(bot, 'CROSS', gap=90)
        note(bot, 'continued')
        for t in range(600):
            yield pad('UP', lx=0.5 * ((t // 120) % 3 - 1))
        note(bot, 'riding again')
        shot(bot, 'riding-again')

    with core.Run(ai.config(name, env, secs=int(os.environ.get('MAX_SECS', '900')), gs=True)) as run:
        bot = ai.Player(run, behaviour)
        core.play(run, ai.read, bot, 20000)
    files = sorted((f, os.path.getsize(os.path.join(photos, f))) for f in os.listdir(photos)) if os.path.isdir(photos) else []
    by = {r['what']: r for r in rows}
    out = dict(cursors=cursors, photos=files)
    if 'photo mode' in by and 'photo left' in by:
        out['frozen'] = by['photo mode']['pos'] == by['photo left']['pos']
    if 'continued' in by and 'riding again' in by:
        out['rode_after'] = by['riding again']['pos'] != by['continued']['pos']
    print(name, json.dumps(out))


if __name__ == '__main__':
    if sys.argv[1:2] == ['ai']:
        w = sys.argv[2:] + [None, None]
        play_ai(w[0], int(w[1]) if w[1] else None, int(w[2]) if w[2] else None)
        sys.exit(0)
    a = sys.argv[1:]
    if a and a[0] == 'play':
        play(a[1], a[2], int(a[3]) if len(a) > 3 else 0)
    else:
        print(__doc__)
