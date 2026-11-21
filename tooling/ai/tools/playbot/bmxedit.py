#!/usr/bin/env python3
"""BMX XXX's Park Editor, played (test only: no retail menu leads to it).

    bmxedit.py play NAME [SEED [EDITS]]   a session into $BMXCORPUS/sessions/NAME (script.txt, meta.json, stats.json,
                                          play.log), as bmx.py's sessions: bmxroutes plays it back as a route
    BMXEDIT_MENU=1: in by the app's main menu line (ZX_BMX_PARK_EDITOR=1) instead of the poke, the park saved as a
    native file, and after EXIT the park ridden from PARK EDITOR, RIDE PARK. GS=1: pictures at the menus.

The editor is AI's ParkEditor in BMX's game mode 2 (the mode table at 0x258778: init 0x1927a8, frame 0x1928c8);
setGameMode (0x190B60) is called only by the Art Viewer's menu (0x191ab8, "Park Editor Test" in UI_AVMenu) and code
no retail menu reaches. The route enters it the way setGameMode does: the mode byte 0x2A2B30 (gp+0x15C0) = 2, the
switch state 0x2A2B32 = 1 and the mode's frame count 0x2A2B34 = 0, poked at tick ENTRY from the main menu. From there
on it is the editor's own menus and tools, closed on the game's state (the mode byte, the rider):

- edits: the tool roll (R1/L1), the cursor (d-pad), the terrain raised and lowered (X, d-pad held, O), pieces rotated
  (O), placed (X) and some deleted again (Square);
- a test run (the pause's TEST RUN) ridden blind (pedal, steer, hops; the park has no map) and back to the editor
  (RETURN TO EDITOR, EXIT? YES);
- more edits, then SAVE: card slot 1, NEW, the description and author keyboards' END, Yes: the park on the card;
- CLEAR PARK, and EXIT to the main menu.

Its numbers: pieces placed and deleted, terrain moves, the test run's distance and jumps, the park file's size on the
card (the run's mc0), and the editor's own count (showEditMemory 0x2197c0's argument, ZX_FNCYCLES_ARGS).
"""
import json
import os
import random
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import bmx  # noqa: E402
from core import pad, NOTHING  # noqa: E402

GAME_MODE = 0x2A2B30          # gp+0x15C0: the game mode (0 front end, 1 game, 2 park editor, 3 a park played), +1 the last, +2 the switch
SHOW_EDIT_MEMORY = 0x2197C0   # the editor's memory line (AI's showEditMemory: its count)
ENTRY = 4300                  # the poke's tick: the main menu is up (the title's START at script 1640)
PARK_EDITOR = 2


def entry_env(at=ENTRY):
    return {'ZX_POKE_SCRIPT': '%x:%x=%x,%x:%x=0' % (at, GAME_MODE, 0x10000 | PARK_EDITOR, at, GAME_MODE + 4)}


class Bot:
    """A generator of pads, run one pad a tick; `st` is the tick's state."""

    def __init__(self, run, seed, log):
        self.run, self.rng, self.log = run, random.Random(seed), log
        self.st = None
        self.stats = core.Stats()

    def game_mode(self):
        return self.run.mem(GAME_MODE, 1)[0]

    def tap(self, button, hold=6, gap=40):
        for _ in range(hold):
            yield pad(button)
        for _ in range(gap):
            yield NOTHING

    def hold(self, frames, p):
        for _ in range(frames):
            yield p

    def wait(self, frames):
        for _ in range(frames):
            yield NOTHING

    def until(self, cond, limit):
        for _ in range(limit):
            if cond(self.st):
                return True
            yield NOTHING
        return False

    def editing(self, st):
        return self.game_mode() == PARK_EDITOR and st.mode == 2 and not st.rider

    def edits(self, n):
        """n edits: a tool, a place, a piece placed (rotated first now and then), some deleted again."""
        rng = self.rng
        for k in range(n):
            if not self.editing(self.st):
                yield from self.until(self.editing, 600)
            for _ in range(rng.randint(1, 5)):
                yield from self.tap(rng.choice(('R1', 'R1', 'L1')), gap=30)
            for _ in range(rng.randint(1, 4)):
                yield from self.tap(rng.choice(('UP', 'DOWN', 'LEFT', 'RIGHT')), hold=8, gap=12)
            for _ in range(rng.randint(0, 2)):
                yield from self.tap('CIRCLE', gap=20)
            yield from self.tap('CROSS', gap=40)
            self.stats.count('placed')
            if rng.random() < 0.25:
                yield from self.tap('SQUARE', gap=40)
                self.stats.count('deleted')
            self.log(self.st, 'edit %d' % k)

    def terrain(self):
        """The terrain tool (the roll's first, where the editor opens): select, raise, drop; select, lower, drop."""
        for d in ('UP', 'DOWN'):
            yield from self.tap('CROSS', gap=20)
            yield from self.hold(30, pad(d))
            yield from self.wait(10)
            yield from self.tap('CIRCLE', gap=30)
            self.stats.count('terrain')
        self.log(self.st, 'terrain raised and lowered')

    def tool_tour(self, tools=12):
        """(routes2) Every tool of the roll in turn (R1), each given every button: the d-pad's four ways tapped, the
        shoulders L2 and R2 (the tool's pieces), CIRCLE (rotate), CROSS (place), TRIANGLE, SQUARE (delete); the view
        turned, tilted and zoomed with the right stick and with the shoulders held on the d-pad."""
        for k in range(tools):
            if not self.editing(self.st):
                yield from self.until(self.editing, 600)
            yield from self.tap('R1', gap=40)
            for b in ('UP', 'RIGHT', 'DOWN', 'LEFT', 'R2', 'R2', 'L2', 'CIRCLE', 'CROSS', 'UP', 'UP', 'CROSS', 'TRIANGLE',
                      'LEFT', 'CROSS', 'SQUARE'):
                yield from self.tap(b, hold=6, gap=24)
            self.stats.count('tools_toured')
            self.log(self.st, 'tool %d toured' % k)
            if k % 4 == 0:
                for rx, ry in ((1.0, 0.0), (-1.0, 0.0), (0.0, 1.0), (0.0, -1.0)):
                    yield from self.hold(30, pad(rx=rx, ry=ry))
                for sh in ('L2', 'R2', 'L1'):
                    for d in ('UP', 'LEFT'):
                        yield from self.hold(20, pad(sh, d))
                        yield from self.wait(10)
                yield from self.wait(30)
                self.stats.count('views')

    def pause_item(self, k):
        yield from self.tap('START', gap=90)
        for _ in range(k):
            yield from self.tap('DOWN', hold=6, gap=25)
        yield from self.tap('CROSS', hold=10, gap=60)

    def test_run(self, ticks):
        yield from self.pause_item(1)                  # TEST RUN
        ok = yield from self.until(lambda s: s.rider, 3000)
        for _ in range(40):                           # the park's dialog (the rider at 0, 0, 0 behind it): CONTINUE
            if self.st.pos != (0.0, 0.0, 0.0):
                break
            yield from self.tap('CROSS', gap=40)
        ok = ok and self.st.pos != (0.0, 0.0, 0.0)
        self.log(self.st, 'test run: rider %s' % ('in the park' if ok else 'NOT there'))
        if not ok:
            return
        meter = bmx_meter()
        rng = self.rng
        for t in range(ticks):
            s = self.st
            meter.tick(s)
            lx = 0.6 * ((t // 150) % 3 - 1)
            if t % 240 < 8 and rng.random() < 0.9:
                yield pad('UP', 'CROSS', lx=lx)         # a hop
            else:
                yield pad('UP', lx=lx)
        for k in ('distance', 'jumps', 'score'):
            self.stats.best('test_' + k, meter.s[k] if k in meter.s.json() else 0)
        self.log(self.st, 'test run over: %s' % json.dumps({k: v for k, v in meter.s.json().items() if k in ('distance', 'jumps', 'score')}))
        # the test run's pause is the game's own (PAUSED: CONTINUE, OPTIONS, RESTART, RETURN TO EDITOR), not UILIB's
        # PauseMenu.RunPause: its fourth line (the third was RESTART: the run began again and never left), then
        # EXIT? (NO above YES)
        yield from self.tap('START', gap=90)
        self.shot('run-pause')
        for _ in range(3):
            yield from self.tap('DOWN', hold=6, gap=25)
        self.shot('run-pause-return')
        yield from self.tap('CROSS', hold=10, gap=90)
        self.shot('exit-confirm')
        yield from self.tap('DOWN', gap=25)
        yield from self.tap('CROSS', hold=10, gap=60)
        self.shot('after-return')
        back = yield from self.until(self.editing, 3000)
        self.shot('back')
        self.log(self.st, 'back in the editor' if back else 'NOT back in the editor')

    def save(self):
        yield from self.pause_item(2)                  # SAVE: slot 1, NEW, description END, author END, Yes
        yield from self.wait(200)
        for gap in (300, 300, 300, 600):
            yield from self.tap('CROSS', hold=10, gap=gap)
        yield from self.tap('UP', gap=40)
        yield from self.tap('CROSS', hold=10, gap=900)
        self.log(self.st, 'saved')

    def clear_and_exit(self):
        yield from self.until(self.editing, 600)
        yield from self.pause_item(3)                  # CLEAR PARK: the confirm, YES above NO
        yield from self.tap('UP', gap=40)
        yield from self.tap('CROSS', hold=10, gap=300)
        self.log(self.st, 'cleared')
        yield from self.until(self.editing, 600)
        yield from self.pause_item(5)                  # EXIT: YES above NO
        yield from self.tap('UP', gap=40)
        yield from self.tap('CROSS', hold=10, gap=60)
        out = yield from self.until(lambda s: s.mode == 1 and self.game_mode() == 0, 3000)
        self.log(self.st, 'the main menu again' if out else 'NOT at the main menu')

    def shot(self, what):
        if os.environ.get('GS') == '1':
            self.log(self.st, 'shot', self.run.shot(what))

    def menu_entry(self):
        """(BMXEDIT_MENU=1) The main menu's PARK EDITOR line (ZX_BMX_PARK_EDITOR=1: third, after MULTIPLAYER), NEW
        PARK, the first theme: the game's own way into the editor."""
        yield from self.until(lambda s: s.mode == 1, 3000)
        yield from self.wait(240)
        self.shot('main-menu')
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('CROSS', hold=8, gap=200)
        self.shot('park-editor-menu')
        yield from self.tap('CROSS', hold=8, gap=200)       # NEW PARK
        self.shot('themes')
        yield from self.tap('CROSS', hold=8, gap=300)       # the first theme

    def ride_saved(self, ticks):
        """(BMXEDIT_MENU=1) PARK EDITOR, RIDE PARK (its third line), the first park, ridden: pedal, steer, hops."""
        yield from self.wait(240)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('CROSS', hold=8, gap=200)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('DOWN', gap=40)
        self.shot('ride-park-line')
        yield from self.tap('CROSS', hold=8, gap=300)       # RIDE PARK: the parks saved
        self.shot('parks-list')
        yield from self.tap('CROSS', hold=8, gap=300)       # the first
        ok = yield from self.until(lambda s: s.rider and s.mode == 2, 3000)
        for _ in range(40):
            if self.st.pos != (0.0, 0.0, 0.0):
                break
            yield from self.tap('CROSS', gap=40)
        self.shot('park-ridden')
        ok = ok and self.st.pos != (0.0, 0.0, 0.0)
        self.log(self.st, 'saved park ridden: %s (game mode %d)' % ('in the park' if ok else 'NOT there', self.game_mode()))
        self.stats.count('ride_park_' + ('ok' if ok else 'failed'))
        if not ok:
            return
        meter = bmx_meter()
        for t in range(ticks):
            meter.tick(self.st)
            lx = 0.6 * ((t // 150) % 3 - 1)
            yield pad('UP', 'CROSS', lx=lx) if t % 240 < 8 else pad('UP', lx=lx)
        for k in ('distance', 'jumps'):
            self.stats.best('ride_' + k, meter.s[k] if k in meter.s.json() else 0)
        self.log(self.st, 'saved park ride over: %s' % json.dumps({k: v for k, v in meter.s.json().items() if k in ('distance', 'jumps', 'score')}))

    def behaviour(self, edits):
        menu = os.environ.get('BMXEDIT_MENU') == '1'
        if menu:
            yield from self.menu_entry()
        ok = yield from self.until(lambda s: self.game_mode() == PARK_EDITOR, ENTRY + 3000)
        if not ok:
            self.log(self.st, 'NOT in the park editor')
            return
        yield from self.until(self.editing, 3000)
        yield from self.wait(300)
        self.log(self.st, 'in the park editor')
        yield from self.terrain()
        if os.environ.get('TOOL_TOUR') == '1':
            yield from self.tool_tour()
        yield from self.edits(edits)
        yield from self.test_run(1800)
        yield from self.edits(edits // 2)
        yield from self.save()
        self.shot('after-save')
        yield from self.clear_and_exit()
        if menu:
            yield from self.ride_saved(1800)


def bmx_meter():
    import dm2
    return dm2.Meter()


def play_session(name, seed=1, edits=16, max_secs=1500):
    import dm2  # noqa: F401  (puts the dm2 tools, bmxroutes among them, on the path)
    import bmxroutes
    menu = bmxroutes.Script()
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    if os.environ.get('BMXEDIT_MENU') == '1':
        # the retail way in (the main menu's PARK EDITOR line, the app's setting) and the app's native park files
        env = dict(ZX_EE_SCALE='4096', ZX_BMX_PARK_EDITOR='1')
    else:
        env = dict(entry_env(), ZX_EE_SCALE='4096', ZX_SAVES='card')
    trace_args = os.path.join(d, 'memory-args.txt')
    run_env = dict(env, ZX_FNCYCLES='0x%x:%s' % (SHOW_EDIT_MEMORY, os.path.join(d, 'memory-calls.txt')), ZX_FNCYCLES_ARGS=trace_args)
    cfg = bmx.run_config(name, menu.text(), env=run_env, max_secs=max_secs, gs=os.environ.get('GS') == '1')
    import player_dm2
    log = player_dm2.Log(os.path.join(d, 'play.log'))
    rec = core.Recorder()
    with core.Run(cfg) as run:
        bot = Bot(run, seed, log)
        program = bot.behaviour(edits)

        def policy(st):
            bot.st = st
            if st.script < menu.t + 30:
                return NOTHING
            try:
                return next(program)
            except StopIteration:
                return None

        core.play(run, bmx.read, policy, max_ticks=200000, recorder=rec)
        card = os.path.join(run.dir, 'mc0')
        if os.environ.get('BMXEDIT_MENU') == '1':
            card = os.path.join(run.dir, 'mc0-saves')     # (native files: <slot 1>-saves/Parks/Park NNN.sav)
    parks = []
    for root, _, files in os.walk(card):
        for f in files:
            if 'PARK' in f.upper() and not f.endswith(('.ico', '.sys')):
                parks.append((f, os.path.getsize(os.path.join(root, f))))
    counts = []
    if os.path.exists(trace_args):
        for line in open(trace_args):
            w = line.split()
            # (address, call, tick, cycle, a0, a1: the count); the runtime's lines that carry no arguments (a direct
            # call's, its sp=/ctxsp= words, a return) are not counts
            if len(w) > 5 and 'ret' not in w and 'direct' not in w and w[0].lstrip('0') == '%x' % SHOW_EDIT_MEMORY:
                try:
                    counts.append(int(w[5], 16))
                except ValueError:
                    continue
    text = menu.text() + ',' + rec.text()
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(text)
    stats = bot.stats.json()
    stats['parks_saved'] = parks
    stats['editor_count_max'] = max(counts) if counts else None
    stats['editor_count_calls'] = len(counts)
    meta = dict(name=name, seed=seed, edits=edits, end=run.script_time, end_tick=run.tick, env=env,
                note='test only: the park editor entered by the game mode poked (no retail menu leads to it); then played')
    core.save_json(os.path.join(d, 'stats.json'), stats)
    core.save_json(os.path.join(d, 'meta.json'), meta)
    print(name, json.dumps(stats))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'play':
        play_session(a[1], int(a[2]) if len(a) > 2 else 1, int(a[3]) if len(a) > 3 else 16,
                     int(os.environ.get('MAX_SECS', '1500')))
    else:
        print(__doc__)
