#!/usr/bin/env python3
"""Scripted routes through Dave Mirra BMX 2 for recording and coverage (the counterpart of the
Aggressive Inline repo's tools/routes.py; docs/READABLE.md "How a function is proven").

Drives a native runner through ZX_CONTROL, closed loop on the game's state: every step waits on
a guest word (mode, level frame, the menu on screen), never on host time.

    dm2routes.py start NAME [KEY=VAL ...]   launch RUNNER with a scratch HOME, card, support folder
                                            under $ROUTES_DIR/NAME; a fresh card unless ROUTE_CARD=DIR
    dm2routes.py NAME CMD [ARGS] [; CMD ...]
    dm2routes.py stop NAME                  stop the runner (by its PID)

The commands are the functions in COMMANDS below, each with its one-line doc; `dm2routes.py help`
lists them. Raw runtime commands (peek, press, shot, ...) pass through when prefixed with `raw`.

Environment: RUNNER, ROUTES_DIR (default $ZAXIS_SCRATCH/dm2corpus/routes), ELF.
"""
import json
import os
import random
import re
import signal
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nctl  # noqa: E402

ROOT = os.path.join(os.path.dirname(HERE), 'workspace')   # (the game's workspace: recomp/)
ELF = os.environ.get('ELF', os.path.join(ROOT, 'recomp', 'disc-root', 'SLUS_201.59'))
def _scratch():
    """paths.env's ZAXIS_SCRATCH (the user's own home, never a scratch HOME's: a gate's runs boot BMX XXX under one)."""
    try:
        sys.path.insert(0, os.path.join(HERE, '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
        import zpaths
        return zpaths.ZAXIS_SCRATCH, zpaths.PATHS.get('BMX_DATA', os.path.join(zpaths.ZAXIS_SCRATCH, 'zbind1', 'bmx'))
    except Exception:
        return os.path.join(os.path.expanduser('~'), 'z-axis-scratch'), os.path.join(os.path.expanduser('~'), 'z-axis-scratch', 'zbind1', 'bmx')


SCRATCH, BMX_DATA = _scratch()
ROUTES_DIR = os.environ.get('ROUTES_DIR', os.path.join(SCRATCH, 'dm2corpus', 'routes'))

# DM2 anchors (tools/PCSX2-DM2.md)
MODE = 0x259770          # 0 boot and movies, 1 front end, 2 loading and level, 3 paused
LEVEL_FRAME = 0x25BCC4   # rider task frames since the level started; 0 in the front end
SESSION = 0x2566C8       # bytes: +1 rider, +2 level, +3 players
LEVEL_CURSOR = 0x25A1A8
RIDER_TABLE = 0x2F4EA0
RIDER_POS, RIDER_VEL = 0x1040, 0x10A0


def folder(name):
    return os.path.join(ROUTES_DIR, name)


nctl.ctl_dir = lambda name: os.path.join(folder(name), 'ctl')


def start(name, pairs):
    d = folder(name)
    ctl = nctl.ctl_dir(name)
    for sub in ('ctl', 'home', 'cwd', 'app', 'shots', 'mc0', 'mc1'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    for f in ('cmd', 'out'):
        open(os.path.join(ctl, f), 'w').close()
    env = dict(os.environ)
    env.setdefault('ZX_DEBUG_UI', '0')
    env.setdefault('ZX_GS_BACKEND', 'cpu')
    env.setdefault('ZX_UNPACED', '1')
    for kv in pairs:
        k, v = kv.split('=', 1)
        env[k] = v
    env.update({'ZX_CONTROL': ctl, 'HOME': os.path.join(d, 'home'), 'ZX_APP_SUPPORT_DIR': os.path.join(d, 'app'),
                'ZX_APP_LOG_DIR': os.path.join(d, 'app'), 'ZX_SCREENSHOT_DIR': os.path.join(d, 'shots'),
                'ZX_MC_ROOT': os.path.join(d, 'mc0')})
    with open(os.path.join(d, 'run.log'), 'w') as log:
        p = subprocess.Popen([os.environ['RUNNER'], ELF], cwd=os.path.join(d, 'cwd'), env=env, stdout=log,
                             stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
    open(os.path.join(ctl, 'pid'), 'w').write(str(p.pid))
    # Every run has a time limit, whatever becomes of the script that started it.
    limit = int(os.environ.get('DM2_ROUTE_SECS', '1800'))
    runner = os.path.basename(os.environ['RUNNER'])
    subprocess.Popen(['/bin/sh', '-c', 'sleep %d; ps -p %d -o command= | grep -q %s && kill %d' % (limit, p.pid, runner, p.pid)],
                     start_new_session=True,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
    print('started pid', p.pid)


def stop(name):
    try:
        pid = int(open(os.path.join(nctl.ctl_dir(name), 'pid')).read())
        os.kill(pid, signal.SIGTERM)
        for _ in range(40):
            time.sleep(0.25)
            os.kill(pid, 0)
        os.kill(pid, signal.SIGKILL)
    except (OSError, ValueError):
        pass


class Game:
    def __init__(self, name):
        self.name = name

    def raw(self, *commands, timeout=None):
        """Run runtime commands; the replies."""
        out = []
        nctl.send(self.name, list(commands), timeout=timeout or 3600, quiet=True, replies=out)
        return out

    def peek(self, address, words=1):
        reply = self.raw('peek 0x%X %d' % (address, words))[0]
        return [int(x, 16) for x in re.findall(r'\b[0-9a-fA-F]{8}\b', reply)][-words:]

    def peekf(self, address, words=1):
        reply = self.raw('peekf 0x%X %d' % (address, words))[0]
        return [float(x) for x in reply.split()[-words:]]

    def mode(self):
        return self.peek(MODE)[0]

    def frame(self):
        return self.peek(LEVEL_FRAME)[0]

    def wait(self, vsyncs):
        self.raw('wait %d' % vsyncs)

    def press(self, button, hold=6, gap=20):
        self.raw('press %s %d %d' % (button, hold, gap))

    def until(self, cond, timeout=6000):
        return 'ok' in self.raw('until %s %d' % (cond, timeout))[0]

    def rider(self):
        table = self.peek(RIDER_TABLE)[0]
        if not 0x100000 <= table < 0x2000000:
            return None
        return table

    def position(self):
        rider = self.rider()
        return self.peekf(rider + RIDER_POS, 3) if rider else None


COMMANDS = {}


def command(f):
    COMMANDS[f.__name__] = f
    return f


@command
def boot(g):
    """intro skipped, to the main menu"""
    g.until('0x%X==0' % MODE, 100)
    for _ in range(200):
        g.press('START', 6, 60)
        if g.mode() == 1:
            break
    g.wait(180)
    print('main menu: mode', g.mode())


@command
def shot(g, name):
    """a screenshot in the route's folder (a run with the GS on)"""
    path = os.path.join(folder(g.name), 'shots', name + '.png')
    g.raw('shot ' + path)
    print(path)


@command
def p(g, *buttons):
    """press these buttons one after another, 40 vsyncs apart"""
    for b in buttons:
        g.press(b, 6, 40)


@command
def w(g, vsyncs):
    """wait"""
    g.wait(int(vsyncs))


@command
def state(g):
    """the anchors"""
    print('mode', g.mode(), 'frame', g.frame(), 'session %08x' % g.peek(SESSION)[0], 'pos', g.position())


# ---- pad script routes ---------------------------------------------------------------------

class Script:
    """A ZX_PAD_SCRIPT, vsync clock: closed on the game by SYNC anchors, so one route replays
    exactly at any host speed (the recorder's, the A/B's)."""

    def __init__(self):
        self.items = ['1969v:START:6v', '2406v:SYNC=0x%X==1' % MODE]
        self.t = 2406 + 158

    def press(self, button, hold=6, after=30):
        self.items.append('%dv:%s:%dv' % (self.t, button, hold))
        self.t += hold + after

    def presses(self, *buttons, after=30):
        for b in buttons:
            self.press(b, after=after)

    def wait(self, vsyncs):
        self.t += vsyncs

    def sync(self, condition):
        self.items.append('%dv:SYNC=%s' % (self.t, condition))

    def hold(self, condition):
        self.items.append('%dv:WAIT=%s' % (self.t, condition))

    def add(self, items):
        self.items += items
        self.t = max([self.t] + [int(i.split('v')[0]) for i in items]) + 30

    def text(self):
        return ','.join(self.items)


# The main menu, top to bottom; its cursor starts on PROQUEST.
MAIN = {'proquest': 0, 'session': 1, 'freeride': 2, 'editor': 3, 'options': 4, 'cool': 5, 'multiplayer': 6}
LEVELS = {0: 'woodward', 1: 'trainyards', 2: 'swamp', 3: 'commercial', 4: 'galloon', 5: 'hwy47', 6: 'devils', 7: 'airport'}


def to_level(s, mode, level=0, rider=0, bike=0, modifier=0):
    """From the main menu, MODE's way to a level: rider, bike, modifiers, level select (menu value)."""
    s.presses(*['DOWN'] * MAIN[mode])
    s.press('CROSS', after=240)
    s.presses(*['RIGHT'] * rider)
    s.press('CROSS', after=240)
    s.presses(*['RIGHT'] * bike)
    s.press('CROSS', after=240)
    s.presses(*['RIGHT'] * modifier)
    s.press('CROSS', after=240)
    s.presses(*['RIGHT'] * level)
    s.press('CROSS', after=400)
    s.sync('0x%X==121' % LEVEL_FRAME)


def ride(s, cycles, seed, only=None):
    import dm2ride
    s.add(dm2ride.ride(s.t + 60, cycles, seed, only))


ROUTES = {}
ENVIRONMENT = {}
CAREER = 0x2174B4        # the riders' records (0x3C8 each); a level's progress at +0x13C + level x 0x44
# The level select's value -> the level's number (0x25A1C8).
LEVEL_BLOCKS = {0: 2, 1: 1, 2: 0, 3: 3, 4: 6, 5: 5, 6: 4, 7: 7}


def freeride(level, cycles, seed):
    s = Script()
    to_level(s, 'freeride', level)
    ride(s, cycles, seed)
    pause_tour(s)
    return s


def scroll(s):
    s.presses('DOWN', 'DOWN', 'DOWN', 'RIGHT', 'RIGHT', 'UP', 'LEFT', after=20)


def pause_tour(s, challenges=False):
    """The pause menu, each item and back (the cursor comes back to CONTINUE), then END RUN: EXIT?, YES."""
    s.press('START', after=60)
    first = 2 if challenges else 1
    if challenges:
        s.press('DOWN', after=30)       # VIEW CHALLENGES
        s.press('CROSS', after=120)
        s.presses('CROSS', 'CROSS', after=120)
        s.press('TRIANGLE', after=60)
        s.press('START', after=60)
        s.wait(30)
        s.press('START', after=60)
    for item in range(first, first + 3):  # OPTIONS, TRICK LIST, VIEW RECORDS
        s.presses(*['DOWN'] * item, after=20)
        s.press('CROSS', after=120)
        scroll(s)
        s.press('TRIANGLE', after=60)
    s.press('UP', after=30)             # the cursor wraps to END RUN
    s.press('CROSS', after=60)
    s.press('DOWN', after=30)
    s.press('CROSS', after=300)
    gameover_tour(s)


def gameover_tour(s):
    """RUN FINISHED: VIEW REPLAY, TRICK LIST, EXIT TO MAIN MENU (the cursor wraps to it), EXIT?, YES."""
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=500)
    s.press('START', after=200)
    s.presses(*['DOWN'] * 3, after=20)
    s.press('CROSS', after=120)
    scroll(s)
    s.press('TRIANGLE', after=100)
    s.press('UP', after=30)
    s.press('CROSS', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=400)


def proquest(level, cycles, seed):
    s = Script()
    to_level(s, 'proquest', level)
    s.wait(134)
    s.press('CROSS', after=200)         # INSTRUCTIONS
    s.press('CROSS', after=200)         # CHALLENGES
    s.press('CROSS', after=200)
    ride(s, cycles, seed, list(range(29)))
    pause_tour(s, challenges=True)
    return s


def session(level, cycles, seed):
    """A timed run (three minutes) to its end, then the run's results."""
    s = Script()
    to_level(s, 'session', level)
    s.wait(134)
    s.press('CROSS', after=200)
    s.press('CROSS', after=200)
    ride(s, cycles, seed)
    s.wait(900)
    s.press('CROSS', after=300)
    gameover_tour(s)
    return s


GAME_TYPES = ('bestrun', 'highfive', 'bmx', 'randomspot', 'wipeout', 'longestgrind')


def multiplayer(kind, level, cycles, seed):
    """MULTIPLAYER (a second pad: port 1 mirrors): both riders, both bikes, modifiers, level, a game."""
    s = multiplayer_menu(kind, level)
    ride(s, cycles, seed)
    s.wait(600)
    for _ in range(10):
        s.press('CROSS', after=120)
    return s


def multiplayer_menu(kind, level, s=None):
    """MULTIPLAYER's menus to the game's first turn: both riders, both bikes, modifiers, level, the game type."""
    s = s or Script()
    s.presses(*['DOWN'] * MAIN['multiplayer'])
    s.press('CROSS', after=200)
    s.press('CROSS', after=200)         # rider 1
    s.press('CROSS', after=200)         # rider 2
    s.press('CROSS', after=200)         # bike 1
    s.press('CROSS', after=200)         # bike 2
    s.press('CROSS', after=200)         # modifiers
    s.presses(*['RIGHT'] * level)
    s.press('CROSS', after=400)         # level
    s.sync('0x%X==121' % LEVEL_FRAME)
    s.wait(120)
    s.presses(*['DOWN'] * kind)
    if GAME_TYPES[kind] == 'bmx':
        # (routes2) B-M-X opens SELECT WORD (a keyboard, the word BMX in it): END as it is (from A: LEFT to the last
        # column, DOWN past the bottom row), then READY! PLAYER ONE: CONTINUE. Its three CROSSes typed "AA" instead.
        s.press('CROSS', after=300)
        if os.environ.get('HORSE_TRIES') == '1':
            # (routes3) the word box's cancel (TRIANGLE: back to SELECT GAME, the cursor on B-M-X), B-M-X again, the
            # word deleted (SQUARE three times) and END on the empty box (it stays)
            s.press('TRIANGLE', after=200)
            s.press('CROSS', after=300)
            s.presses('SQUARE', 'SQUARE', 'SQUARE', after=20)
            s.press('LEFT', after=20)
            s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=20)
            s.press('CROSS', after=120)
            word = os.environ.get('HORSE_WORD', 'L')
            # (from END, the bottom right: UP four times is L; the word typed from there, END again)
            assert word == 'L', 'HORSE_WORD: only L for now'
            s.presses('UP', 'UP', 'UP', 'UP', after=20)
            s.press('CROSS', after=30)
            s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=20)
            s.press('CROSS', after=300)
            s.press('CROSS', after=200)
            return s
        s.press('LEFT', after=20)
        s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=20)
        s.press('CROSS', after=300)
        s.press('CROSS', after=200)
        return s
    s.press('CROSS', after=200)
    s.press('CROSS', after=200)
    s.press('CROSS', after=200)
    return s


def frontend():
    """Every front-end screen: options, the card (save, load), records, movies, credits, replays, rider and bike lists."""
    s = Script()
    s.presses(*['DOWN'] * MAIN['options'])
    s.press('CROSS', after=120)
    for item in (0, 1):                 # CONTROLLER 1, 2: the configs and vibration
        s.press('CROSS', after=120)
        s.presses('RIGHT', 'RIGHT', 'RIGHT', 'RIGHT', 'DOWN', 'RIGHT', 'RIGHT', after=30)
        s.press('TRIANGLE', after=60)
        s.press('DOWN', after=30)
    s.press('CROSS', after=120)         # AUDIO
    s.presses('LEFT', 'LEFT', 'RIGHT', 'RIGHT', 'DOWN', 'LEFT', 'LEFT', 'RIGHT', 'RIGHT', after=30)
    s.press('TRIANGLE', after=60)
    s.press('DOWN', after=30)
    s.press('CROSS', after=120)         # LOAD GAME: slot 1, the card is looked at
    s.press('CROSS', after=300)
    s.presses('TRIANGLE', 'TRIANGLE', after=120)
    s.wait(120)
    s.press('CROSS', after=120)         # (the cursor stays on LOAD GAME)
    s.press('TRIANGLE', after=120)
    s.press('DOWN', after=30)
    s.press('CROSS', after=120)         # SAVE GAME: slot 1, SAVE
    s.press('CROSS', after=200)
    s.press('UP', after=30)
    s.press('CROSS', after=500)
    s.press('CROSS', after=300)         # SAVE COMPLETE: back to the main menu, on OPTIONS
    s.press('CROSS', after=120)         # LOAD GAME: slot 1, LOAD
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=200)
    s.press('CROSS', after=300)
    s.press('UP', after=30)
    s.press('CROSS', after=500)
    s.press('CROSS', after=300)
    return s


def cool():
    """COOL STUFF: level records (every level), the movies, credits, replays."""
    s = Script()
    s.presses(*['DOWN'] * MAIN['cool'])
    s.press('CROSS', after=120)
    s.press('CROSS', after=200)         # LEVEL RECORDS
    for _ in range(9):
        s.presses('DOWN', 'DOWN', 'UP', after=20)
        s.press('RIGHT', after=60)
    s.press('TRIANGLE', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=200)         # MOVIES: the first three, each for a while
    for _ in range(3):
        s.press('CROSS', after=900)
        s.press('START', after=100)
        s.press('TRIANGLE', after=60)
        s.press('DOWN', after=30)
    s.press('TRIANGLE', after=100)
    s.press('TRIANGLE', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=1500)        # CREDITS
    s.press('START', after=100)
    s.press('TRIANGLE', after=100)
    s.press('TRIANGLE', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=300)         # VIEW REPLAYS
    s.press('CROSS', after=300)
    s.press('CROSS', after=300)
    return s


def editor(theme, moves, seed):
    """PARK EDITOR: a theme, then seeded edits (cursor, pieces, categories, place, drop, delete, copy), the
    pause menu, and Play Park."""
    rng = random.Random(seed)
    s = Script()
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=200)
    s.press('CROSS', after=300)         # (the first screen)
    s.presses(*['RIGHT'] * theme)
    s.press('CROSS', after=300)
    s.sync('0x%X==0' % MODE)
    s.wait(600)
    for k in range(moves):
        for _ in range(rng.randint(1, 3)):
            s.press(rng.choice(('L1', 'R1', 'L2', 'R2')), after=20)
        for _ in range(rng.randint(1, 8)):
            s.press(rng.choice(('UP', 'DOWN', 'LEFT', 'RIGHT')), hold=8, after=8)
        s.press(rng.choice(('CROSS', 'CROSS', 'CROSS', 'CIRCLE', 'SQUARE', 'TRIANGLE')), after=40)
        if k % 40 == 39:
            s.press('START', after=120)
            s.presses('DOWN', 'DOWN', after=30)
            s.presses('UP', 'UP', after=30)
            s.press('START', after=120)
    s.press('START', after=120)         # TEST RUN: the park ridden, END RUN, a replay, back
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=700)
    s.press('CROSS', after=200)
    ride(s, 6, seed)
    s.press('START', after=100)
    s.press('UP', after=30)
    s.press('CROSS', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=400)
    s.press('DOWN', after=30)           # GAME OVER: VIEW REPLAY
    s.press('CROSS', after=400)
    s.press('START', after=200)
    s.press('UP', after=30)             # RETURN TO LEVEL EDITOR, EXIT?, YES
    s.press('CROSS', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=800)
    s.press('START', after=120)         # LIGHT OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=120)
    scroll(s)
    s.press('TRIANGLE', after=100)
    s.press('START', after=120)
    s.press('START', after=120)
    s.press('DOWN', after=30)           # SAVE PARK
    s.press('CROSS', after=300)
    s.presses('CROSS', 'UP', 'CROSS', after=300)
    s.press('CROSS', after=600)
    s.press('CROSS', after=300)
    s.press('START', after=120)         # CLEAR ALL
    s.press('UP', after=30)
    s.press('UP', after=30)
    s.press('CROSS', after=120)
    s.press('CROSS', after=120)
    s.press('START', after=120)         # EXIT, YES
    s.press('UP', after=30)
    s.press('CROSS', after=120)
    s.press('UP', after=30)
    s.press('CROSS', after=500)
    return s


for _level, _name in LEVELS.items():
    ROUTES['fr-' + _name] = ((lambda l: lambda: freeride(l, 24, 100 + l))(_level), 'Free Ride, ' + _name + ', every ride pattern')
    ROUTES['pq-' + _name] = ((lambda l: lambda: proquest(l, 24, 200 + l))(_level), 'ProQuest, ' + _name + ', the pause menu, End Run')
    ROUTES['se-' + _name] = ((lambda l: lambda: session(l, 36, 300 + l))(_level), 'Session, ' + _name + ', a run to its end, the results')
for _level in (0, 1, 3):
    for _tier in (1, 2, 3):
        _route = 'pq-%s-t%d' % (LEVELS[_level], _tier)
        ROUTES[_route] = ((lambda l, t: lambda: proquest(l, 24, 600 + 10 * l + t))(_level, _tier),
                          'ProQuest, %s, the career poked to tier %d: its goals and objects' % (LEVELS[_level], _tier))
        # (test only, for recording) every rider's progress in that level: open, the tier
        ENVIRONMENT[_route] = {'ZX_POKE_SCRIPT': ','.join(
            '%x:%x=%x' % (2800, CAREER + rider * 0x3C8 + 0x13C + LEVEL_BLOCKS[_level] * 0x44 + 0x3C, 0x0001 | _tier << 8)
            for rider in range(21))}
# (test only, for recording) the run's score (the rider's float at +0x1A0) poked past the rookie challenge's,
# so its end, the rewards and the next tier run; the rider's address as these routes reach it (MIXING 14.20).
for _level, _rider in ((0, 0x1256B10), (1, 0x100D380), (3, 0x12D15C0)):
    _route = 'pq-%s-win' % LEVELS[_level]
    ROUTES[_route] = ((lambda l: lambda: proquest(l, 24, 800 + l))(_level), 'ProQuest, %s, the challenge passed by a poked score' % LEVELS[_level])
    ENVIRONMENT[_route] = {'ZX_POKE_SCRIPT': ','.join('%x:%x=%x' % (t, _rider + 0x1A0, 0x47C35000) for t in (8000, 10000, 12000, 14000))}
ROUTES['gs-woodward'] = ((lambda: freeride(0, 12, 700)), 'Free Ride, Woodward, with the GS and VU1 on (the CPU renderer): the renderers the null GS skips')


def pause_music_track():
    """(verdmbx1) Free Ride, Woodward: the pause menu's OPTIONS, its Music Track row (the fifth: Camera Type,
    Vibration, SFX Volume, Music Volume, Music Track; Music Soundtrack is disabled) stepped RIGHT and LEFT
    (OnChange CALL bmxsoundChangedTrack), back, riding again, then the pause tour."""
    s = Script()
    to_level(s, 'freeride', 0)
    ride(s, 2, 731)
    s.press('START', after=60)
    s.press('DOWN', after=20)              # OPTIONS
    s.press('CROSS', after=120)
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=20)
    s.presses('RIGHT', 'RIGHT', 'LEFT', after=60)
    s.press('TRIANGLE', after=60)
    s.press('START', after=120)            # riding again
    ride(s, 1, 732)
    pause_tour(s)
    return s


ROUTES['vr-pause-music'] = (pause_music_track, "verdmbx1: the pause menu's Music Track row stepped (bmxsoundChangedTrack)")


def no_card_boot():
    """(verdmbx1) A boot with no memory card in either slot (ZX_MC_SLOT1/2=none): the intro's start check reports it,
    answered with its second choice (UP, CROSS: continue without saving), whose onClosed is closeIntroMenu; then the
    main menu's OPTIONS and back."""
    s = Script()
    s.items = ['1969v:START:6v']
    s.t = 2160
    for _ in range(4):                     # (the report's choices: UP to the second, CROSS; again while it is up)
        s.press('UP', after=40)
        s.press('CROSS', after=120)
    s.sync('0x%X==1' % MODE)
    s.wait(200)
    s.presses(*['DOWN'] * MAIN['options'])
    s.press('CROSS', after=200)
    s.press('TRIANGLE', after=200)
    return s


ROUTES['vr-nocard'] = (no_card_boot, 'verdmbx1: a boot with no memory card, the start check answered with continue (closeIntroMenu)')
ENVIRONMENT['vr-nocard'] = {'ZX_MC_SLOT1': 'none', 'ZX_MC_SLOT2': 'none'}
ENVIRONMENT['gs-woodward'] = {'ZX_GS_NULL': None, 'ZX_VU1_SKIP': None}
ROUTES['frontend'] = (frontend, 'options, controllers, audio, the card: load and save')
ROUTES['saves'] = (frontend, 'as frontend with the native save (the game\'s save serialisation; `card` runs the card dialogs)')
ROUTES['cool'] = (cool, 'cool stuff: records, movies, credits, replays')
ROUTES['cool-hle'] = (cool, 'as cool with the game\'s own movie player (ZX_MOVIES=hle: its IPU decoding and IOP sound, not the host\'s)')
ENVIRONMENT['cool-hle'] = {'ZX_MOVIES': 'hle'}
# The app's Settings page in the game's menus (ZX_GAME_SETTINGS_MENUS=1, zge_settings.cpp's page): the rows of each
# of its sections (L1/R1) the runner has, in order (Display, Sound, Controls, Camera, Assists, Cheats: the Controls
# rows from Pedal on are the keys, the pad's buttons and Reset Controls to Defaults, actions that Right and Left leave
# alone). The app has more (display.*); the page's Down wraps.
MENUS_ROWS = (1, 2, 45, 3, 2, 3)


def settings_walk(s, rows):
    """On the Settings page: in each section (`rows`: its rows) every row stepped Right then Left, frames between, then
    Down; R1 to the next section. After the last R1 comes round to the first section, and L1 goes back to the last."""
    for count in rows:
        for _ in range(count):
            s.presses('RIGHT', 'LEFT', after=24)
            s.press('DOWN', after=16)
        s.press('R1', after=30)
    s.press('L1', after=30)


def menus():
    """The Settings page from the front end's Options (its sixth line, SETTINGS) and from the pause menu in Woodward
    Camp's free ride (its last line), every section and row stepped Right and Left; back out each time."""
    s = Script()
    s.presses(*['DOWN'] * MAIN['options'])
    s.press('CROSS', after=120)          # OPTIONS
    s.presses(*['DOWN'] * 5, after=30)   # SETTINGS
    s.press('CROSS', after=120)
    settings_walk(s, MENUS_ROWS)
    s.press('TRIANGLE', after=120)       # Options, on SETTINGS
    s.press('TRIANGLE', after=120)       # the main menu, on OPTIONS
    s.presses('UP', 'UP', after=30)      # FREE RIDE
    s.press('CROSS', after=240)
    for _ in range(4):                   # rider, bike, modifiers, level: Woodward Camp
        s.press('CROSS', after=240)
    s.wait(160)
    s.sync('0x%X==121' % LEVEL_FRAME)
    s.wait(120)
    s.press('START', after=60)           # the pause menu
    s.press('UP', after=30)              # (wraps to its last line, SETTINGS)
    s.press('CROSS', after=120)
    settings_walk(s, MENUS_ROWS)
    s.press('TRIANGLE', after=120)       # the pause menu again
    s.press('START', after=120)          # riding
    s.wait(300)
    return s


ROUTES['menus'] = (menus, "the app's Settings page (ZX_GAME_SETTINGS_MENUS=1) from Options and the pause menu: every section, Right and Left on every row")
ENVIRONMENT['menus'] = {'ZX_GAME_SETTINGS_MENUS': '1', 'ZX_TRACE_SETTINGS': '1'}  # (the trace: the page's keys, which the A/B reads back)
# The game's own exit (test only, as AI's cov-quit): engineMain's loop flag (0x2073B0, the word requestQuit sets; nothing
# in the retail game calls it) poked, so the main loop ends and the game's and the engine's shutdown run, then Exit.
QUIT_FLAG = 0x2073B0


def quit_env(at):
    """The loop flag set at tick `at` (ZX_POKE_SCRIPT), and the route's end 1,800 ticks on: after its Exit the guest
    runs no more (the pad script's clock stops), the host's vsyncs go on at their own pace."""
    return {'ZX_POKE_SCRIPT': '%x:%x=1' % (at, QUIT_FLAG), 'ZX_DUMP_WHEN': 'end:tick>=%d' % (at + 1800)}


def quit_fe():
    """The main menu, then the exit: the shutdown from the front end. The script runs on past the poke, so the
    route's end is the game's own Exit."""
    s = Script()
    s.wait(600)
    s.press('DOWN', after=3000)
    return s


ROUTES['quit-fe'] = (quit_fe, "the game's exit from the main menu (main's loop flag poked: test only): the game and engine shutdown")
ENVIRONMENT['quit-fe'] = quit_env(2406 + 158 + 600)
for _kind, _name in enumerate(GAME_TYPES):
    ROUTES['mp-' + _name] = ((lambda k: lambda: multiplayer(k, k, 34, 400 + k))(_kind), 'Multiplayer, ' + _name)
for _theme in range(4):
    ROUTES['editor-%d' % _theme] = ((lambda t: lambda: editor(t, 120, 500 + t))(_theme), 'Park Editor, theme %d, seeded edits' % _theme)


# ---- routes2: the park editor's tools and the card (pictures checked in explore/dx1) --------------------------------
def keyboard_end(s):
    """A DM2 keyboard (4 rows of 12 and SPC DEL END under them): from the cursor's start (A), one letter typed, then to
    END (the last column, down past the bottom row) and accepted."""
    s.press('CROSS', after=30)
    s.press('LEFT', after=20)
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=20)
    s.press('CROSS', after=200)


def make_gap(s, moves, value_downs=1, cancel=False):
    """The gap tool: CROSS starts a gap, the cursor moved, CROSS ends it: GAP NAME (keyboard), GAP VALUE (a menu)."""
    s.press('CROSS', after=90)
    s.presses(*moves, after=20)
    s.press('CROSS', after=150)
    if cancel:
        s.press('TRIANGLE', after=120)  # (BACKUP: the keyboard closed, no gap)
        return
    keyboard_end(s)
    s.presses(*['DOWN'] * value_downs, after=30)
    s.press('CROSS', after=150)


def save_park(s, first=True):
    """The editor's pause menu (cursor on CONTINUE): SAVE PARK, slot 1, the first row, park name, author, SAVE COMPLETE."""
    s.press('START', after=120)
    s.press('DOWN', after=30)
    s.press('CROSS', after=600)         # SELECT SLOT
    s.press('CROSS', after=1200)        # MEMORY CARD SLOT 1: the parks (the card read: waits kept long, the card's
    s.press('CROSS', after=600)         # the first row: ENTER PARK NAME              work runs on the host's time)
    keyboard_end(s)                     # ENTER PARK AUTHOR NAME
    keyboard_end(s)
    s.wait(6000)                        # SAVE COMPLETE (a recording run writes the card much later than a plain run)
    s.press('CROSS', after=600)         # (the pause menu again, its cursor on SAVE PARK)
    s.press('UP', after=30)
    s.press('CROSS', after=120)         # CONTINUE


def editor_exit(s):
    """The editor's pause menu (cursor on CONTINUE): EXIT, YES, the main menu."""
    s.press('START', after=120)
    s.presses('UP', after=30)           # (wraps to EXIT)
    s.press('CROSS', after=150)
    s.press('UP', after=30)             # YES
    s.press('CROSS', after=1200)


def editor_card():
    """PARK EDITOR, NEW PARK: each tool used (terrain, quarter pipe with its copy, gaps made, one backed out of and one
    deleted, starts, rails, lights), a TEST RUN and back through the game over's VIEW REPLAY, SAVE PARK to the card,
    EXIT; then PARK EDITOR, LOAD PARK (the park saved), an edit, EXIT; FREERIDE's level select past the last
    level."""
    rng = random.Random(901)
    s = Script()
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=300)         # NEW PARK, LOAD PARK
    s.press('CROSS', after=300)         # NEW PARK: the themes
    s.press('CROSS', after=300)         # the theme (the first)
    s.sync('0x%X==0' % MODE)
    s.wait(300)
    # TERRAIN: select, move, drop; raise and lower
    for _ in range(4):
        s.press('CROSS', after=40)
        s.presses(*[rng.choice(('UP', 'DOWN', 'LEFT', 'RIGHT')) for _ in range(3)], after=15)
        s.press('CIRCLE', after=40)
    s.press('R1', after=60)             # QUARTER PIPE 2: placed, rotated, copied, moved, placed
    for _ in range(3):
        s.presses('RIGHT', 'RIGHT', 'RIGHT', after=15)
        s.press('CROSS', after=60)
        s.press('CIRCLE', after=40)
        s.press('TRIANGLE', after=60)
        s.presses('UP', 'UP', after=15)
        s.press('CROSS', after=60)
    s.press('R2', after=40)             # the next piece of the kind
    s.presses('LEFT', 'LEFT', 'LEFT', 'LEFT', after=15)
    s.press('CROSS', after=60)
    s.press('SQUARE', after=60)         # deleted
    s.press('L1', after=60)
    s.press('L1', after=60)             # GAP TOOL
    make_gap(s, ['RIGHT'] * 4, value_downs=1)
    make_gap(s, ['UP'] * 3, value_downs=3)
    make_gap(s, ['LEFT'] * 3, cancel=True)
    s.presses('DOWN', 'DOWN', 'DOWN', after=15)
    s.press('SQUARE', after=90)         # DELETE (a gap end under the cursor, if one is)
    s.press('L1', after=60)             # START TOOL: placed, rotated
    s.presses('DOWN', 'DOWN', after=15)
    s.press('CROSS', after=60)
    s.press('CIRCLE', after=60)
    s.press('L1', after=60)             # RAIL TOOL: placed, rotated, copied, deleted
    for _ in range(2):
        s.presses('LEFT', 'LEFT', after=15)
        s.press('CROSS', after=60)
        s.press('CIRCLE', after=40)
        s.press('TRIANGLE', after=60)
        s.press('DOWN', after=15)
        s.press('CROSS', after=60)
    s.press('SQUARE', after=60)
    for _ in range(3):                  # the tools before it (lights and the rest): each tried
        s.press('L1', after=60)
        s.press('CROSS', after=60)
        s.presses('RIGHT', 'UP', after=15)
        s.press('CROSS', after=60)
        s.press('TRIANGLE', after=60)
    s.press('START', after=120)         # TEST RUN: ridden, END RUN, VIEW REPLAY, back to the editor
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=300)
    s.sync('0x%X==2' % MODE)            # (the run's level: anchors keep the route on the game whatever the host's speed)
    s.wait(600)
    s.press('CROSS', after=200)
    ride(s, 6, 902)
    s.press('START', after=100)
    s.press('UP', after=30)
    s.press('CROSS', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=400)
    s.press('DOWN', after=30)           # GAME OVER: VIEW REPLAY
    s.press('CROSS', after=600)
    s.press('START', after=200)
    s.press('UP', after=30)             # RETURN TO LEVEL EDITOR, EXIT?, YES
    s.press('CROSS', after=100)
    s.press('DOWN', after=30)
    s.press('CROSS', after=300)
    s.sync('0x%X==0' % MODE)            # back in the editor
    s.wait(600)
    save_park(s)
    editor_exit(s)
    s.sync('0x%X==1' % MODE)            # the main menu
    s.wait(300)
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=300)
    s.press('DOWN', after=30)           # LOAD PARK
    s.press('CROSS', after=900)         # SELECT SLOT
    s.press('CROSS', after=1500)        # slot 1: the parks
    s.press('CROSS', after=300)         # the park saved: the editor
    s.sync('0x%X==0' % MODE)
    s.wait(600)
    s.press('L1', after=60)
    s.press('CROSS', after=60)
    editor_exit(s)
    s.sync('0x%X==1' % MODE)
    s.wait(300)
    s.presses(*['DOWN'] * MAIN['freeride'])
    s.press('CROSS', after=300)         # rider, bike, modifiers
    s.presses('CROSS', 'CROSS', 'CROSS', after=300)
    s.press('LEFT', after=200)          # the level select past its first: the park?
    s.press('LEFT', after=200)
    s.press('TRIANGLE', after=200)
    return s


ROUTES['ed-card'] = (editor_card, 'routes2: the park editor tools, gaps, a test run and replay, the park saved and loaded')


# Played routes: real play by tools/playbot/dm2.py (the Aggressive Inline repo) in lock step, kept as pad scripts in
# DM2_PLAY_SESSIONS, each with its meta.json. They replay the played run tick for tick at the clock they were played at.
PLAY_SESSIONS = os.environ.get('DM2_PLAY_SESSIONS', os.path.join(SCRATCH, 'playdm2', 'sessions'))


class Played:
    def __init__(self, path):
        self.path = path

    def text(self):
        with open(self.path) as f:
            return f.read().strip()


def played_routes():
    if not os.path.isdir(PLAY_SESSIONS):
        return
    for name in sorted(os.listdir(PLAY_SESSIONS)):
        script = os.path.join(PLAY_SESSIONS, name, 'route.txt')
        if os.path.exists(script):
            ROUTES['play-' + name] = ((lambda p: lambda: Played(p))(script), 'Real play: ' + name)
            meta = {}
            if os.path.exists(os.path.join(PLAY_SESSIONS, name, 'meta.json')):
                with open(os.path.join(PLAY_SESSIONS, name, 'meta.json')) as f:
                    meta = json.load(f)
            ENVIRONMENT['play-' + name] = dict({'ZX_EE_SCALE': '4096', 'ZX_MC_FIXED_TIME': '1'}, **meta.get('env', {}))


played_routes()


def editor_save():
    """(routes2) PARK EDITOR, NEW PARK, a few pieces and a gap, SAVE PARK, EXIT; PARK EDITOR, LOAD PARK, an edit, EXIT
    (ed-card's card half, without its test run)."""
    s = Script()
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=300)
    s.press('CROSS', after=300)
    s.press('CROSS', after=300)         # the theme
    s.sync('0x%X==0' % MODE)
    s.wait(600)
    s.press('R1', after=60)
    for _ in range(3):
        s.presses('RIGHT', 'RIGHT', 'RIGHT', after=15)
        s.press('CROSS', after=60)
    s.press('L1', after=60)
    s.press('L1', after=60)             # GAP TOOL
    make_gap(s, ['RIGHT'] * 4, value_downs=2)
    s.wait(300)
    save_park(s)
    editor_exit(s)
    s.sync('0x%X==1' % MODE)
    s.wait(600)
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=300)
    s.press('DOWN', after=30)           # LOAD PARK
    s.press('CROSS', after=900)
    s.press('CROSS', after=1500)
    s.press('CROSS', after=300)
    s.sync('0x%X==0' % MODE)
    s.wait(600)
    s.press('L1', after=60)
    s.press('CROSS', after=60)
    editor_exit(s)
    s.sync('0x%X==1' % MODE)
    s.wait(300)
    return s


ROUTES['ed-save'] = (editor_save, 'routes2: the park editor: a park saved to the card and loaded back')


def editor_tour(theme=1, seed=903):
    """(routes3) PARK EDITOR, NEW PARK, the second theme: every tool of the roll (R1 to the next) used the same way: the
    view turned, tilted and zoomed with the right stick (RX, RY held) and L2 / R2, the cursor moved, CROSS (select or
    place) and moved and CROSS again, CIRCLE (drop or rotate), TRIANGLE (copy), SQUARE (delete), a second CROSS and
    SQUARE on the same cell; then the pause menu: LIGHT OPTIONS (each row, both ways), CLEAR ALL (NO, then YES), more
    tools after the clear, SAVE PARK (slot 1, names), EXIT (YES); PARK EDITOR, LOAD PARK (the park), an edit, EXIT."""
    rng = random.Random(seed)
    s = Script()
    s.presses(*['DOWN'] * MAIN['editor'])
    s.press('CROSS', after=300)         # NEW PARK, LOAD PARK
    s.press('CROSS', after=300)         # NEW PARK: the themes
    s.presses(*['RIGHT'] * theme, after=120)
    s.press('CROSS', after=300)
    s.sync('0x%X==0' % MODE)
    s.wait(600)

    def use_tool():
        for stick in ('RX=1', 'RX=-1', 'RY=1', 'RY=-1'):
            s.press(stick, hold=40, after=20)
        s.press('L2', hold=30, after=20)
        s.press('R2', hold=30, after=20)
        for _ in range(2):
            s.presses(*[rng.choice(('UP', 'DOWN', 'LEFT', 'RIGHT')) for _ in range(3)], after=12)
            s.press('CROSS', after=60)
            s.presses(*[rng.choice(('UP', 'DOWN', 'LEFT', 'RIGHT')) for _ in range(2)], after=12)
            s.press('CROSS', after=60)
            s.press('CIRCLE', after=60)
            s.press('TRIANGLE', after=60)
            s.presses('RIGHT', 'RIGHT', after=12)
            s.press('CROSS', after=60)
            s.press('SQUARE', after=60)
            s.press('SQUARE', after=60)
        s.press('TRIANGLE', after=60)
        s.press('CIRCLE', after=60)

    for _ in range(13):
        use_tool()
        s.press('R1', after=90)
    s.press('START', after=120)         # LIGHT OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=150)
    for _ in range(4):
        s.presses('RIGHT', 'RIGHT', 'LEFT', after=20)
        s.press('DOWN', after=20)
    s.press('CROSS', after=100)
    s.press('TRIANGLE', after=120)
    s.press('START', after=120)         # CLEAR ALL: NO, then YES
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=150)
    s.press('CROSS', after=150)
    s.press('CROSS', after=150)
    s.press('UP', after=30)
    s.press('CROSS', after=300)
    s.press('TRIANGLE', after=150)
    for _ in range(5):
        use_tool()
        s.press('L1', after=90)
    s.press('START', after=120)         # SAVE PARK: slot 1, the two names
    s.press('DOWN', after=30)
    s.press('CROSS', after=900)
    s.press('CROSS', after=900)
    keyboard_end(s)
    s.wait(300)
    keyboard_end(s)
    s.wait(1500)
    s.press('CROSS', after=600)
    s.press('START', after=120)         # EXIT, YES
    s.press('UP', after=30)
    s.press('CROSS', after=120)
    s.press('UP', after=30)
    s.press('CROSS', after=600)
    s.sync('0x%X==1' % MODE)
    s.wait(900)
    s.press('CROSS', after=300)         # PARK EDITOR (the cursor kept), LOAD PARK
    s.press('DOWN', after=30)
    s.press('CROSS', after=900)
    s.press('CROSS', after=1500)        # slot 1: the park
    s.press('CROSS', after=1500)
    s.sync('0x%X==0' % MODE)
    s.wait(600)
    use_tool()
    s.press('START', after=120)         # EXIT, YES
    s.press('UP', after=30)
    s.press('CROSS', after=120)
    s.press('UP', after=30)
    s.press('CROSS', after=600)
    s.sync('0x%X==1' % MODE)
    s.wait(600)
    return s


ROUTES['ed-tour'] = (editor_tour, 'routes3: the park editor: every tool, the view, light options, clear all, save, load')
ENVIRONMENT['ed-tour'] = {'ZX_MC_FIXED_TIME': '1'}


def frontend_card():
    """(routes2) The front end's card menus (pictures: explore/dx6): OPTIONS (CONTROLLER 1, CONTROLLER 2 greyed with one
    pad but a cursor stop, AUDIO, LOAD GAME, SAVE GAME): SAVE GAME (SAVE GAME? SAVE / CANCEL), LOAD GAME (LOAD /
    CANCEL), AUDIO, CONTROLLER 1, back; PARK EDITOR, LOAD PARK with no park on the card, back; COOL STUFF, VIEW
    REPLAYS with no replay, back. (`frontend`'s presses take CONTROLLER 2 for a screen and lose their place.)"""
    s = Script()
    s.wait(300)
    s.presses(*['DOWN'] * MAIN['options'], after=30)
    s.press('CROSS', after=600)         # OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # SAVE GAME: SAVE GAME? (on CANCEL)
    s.press('UP', after=30)
    s.press('CROSS', after=2000)        # SAVE COMPLETE
    s.press('CROSS', after=600)
    s.press('UP', after=30)
    s.press('CROSS', after=900)         # LOAD GAME: LOAD GAME? (on CANCEL)
    s.press('UP', after=30)
    s.press('CROSS', after=2000)        # LOAD COMPLETE
    s.press('CROSS', after=600)
    s.press('UP', after=30)
    s.press('CROSS', after=300)         # AUDIO
    s.presses('LEFT', 'RIGHT', 'DOWN', 'LEFT', 'RIGHT', after=30)
    s.press('TRIANGLE', after=200)
    s.presses('UP', 'UP', after=30)
    s.press('CROSS', after=300)         # CONTROLLER 1
    s.presses('RIGHT', 'DOWN', 'RIGHT', after=30)
    s.press('TRIANGLE', after=200)
    s.press('TRIANGLE', after=900)      # the main menu (on OPTIONS)
    s.press('UP', after=30)
    s.press('CROSS', after=600)         # PARK EDITOR
    s.press('DOWN', after=30)
    s.press('CROSS', after=900)         # LOAD PARK: SELECT SLOT
    s.press('CROSS', after=1500)        # slot 1: no park
    s.press('CROSS', after=300)
    s.presses('TRIANGLE', 'TRIANGLE', after=300)
    s.press('TRIANGLE', after=900)
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=600)         # COOL STUFF
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # VIEW REPLAYS: SELECT SLOT
    s.press('CROSS', after=1500)        # slot 1: no replay
    s.press('CROSS', after=300)
    s.presses('TRIANGLE', 'TRIANGLE', after=300)
    s.press('TRIANGLE', after=900)
    return s


ROUTES['fe-card'] = (frontend_card, 'routes2: the front end card menus: save and load game, load park and view replays with none')
ENVIRONMENT['fe-card'] = {'ZX_MC_FIXED_TIME': '1'}


def card_tail(s):
    """(routes2) From a session's RUN FINISHED (its cursor on ANOTHER RUN): SAVE GAME (slot 1, SAVE), SAVE REPLAY (slot 1,
    the first replay slot, a name), EXIT TO MAIN MENU (YES); COOL STUFF, VIEW REPLAYS: the replay saved, loaded from the
    card and watched, START out of it; OPTIONS, LOAD GAME (slot 1, LOAD). Pictures: explore/dx4."""
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # SAVE GAME: SELECT SLOT
    s.press('CROSS', after=900)         # slot 1: SAVE GAME? SAVE / CANCEL (on CANCEL)
    s.press('UP', after=30)
    s.press('CROSS', after=1500)        # SAVE COMPLETE
    s.press('CROSS', after=600)         # RUN FINISHED (on ANOTHER RUN)
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # SAVE REPLAY: SELECT SLOT
    s.press('CROSS', after=1500)        # slot 1: SELECT REPLAY SLOT
    s.press('CROSS', after=900)         # the first: ENTER REPLAY NAME
    keyboard_end(s)
    s.wait(1500)                        # SAVE COMPLETE
    s.press('CROSS', after=600)
    s.press('UP', after=30)             # (wraps to EXIT TO MAIN MENU)
    s.press('CROSS', after=150)         # EXIT? NO / YES
    s.press('DOWN', after=30)
    s.press('CROSS', after=300)
    s.sync('0x%X==1' % MODE)
    s.wait(900)
    s.presses(*['DOWN'] * MAIN['cool'], after=30)
    s.press('CROSS', after=600)         # COOL STUFF
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # VIEW REPLAYS: LOAD REPLAY, SELECT SLOT
    s.press('CROSS', after=1500)        # slot 1: SELECT REPLAY SLOT
    s.press('CROSS', after=300)         # the replay: loaded and played
    s.sync('0x%X==2' % MODE)
    s.wait(1800)
    s.press('START', after=300)         # (PRESS START BUTTON TO EXIT): COOL STUFF
    s.sync('0x%X==0' % MODE)
    s.wait(300)
    s.press('TRIANGLE', after=300)
    s.sync('0x%X==1' % MODE)
    s.wait(900)
    s.presses(*['DOWN'] * MAIN['options'], after=30)
    s.press('CROSS', after=300)         # OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)         # LOAD GAME: SELECT SLOT
    s.press('CROSS', after=900)         # slot 1: LOAD / CANCEL
    s.press('UP', after=30)
    s.press('CROSS', after=1500)
    s.press('CROSS', after=600)
    s.press('TRIANGLE', after=300)


class PlayedTail:
    """A played session's script (DM2_PLAY_SESSIONS) and a tail from its end."""

    def __init__(self, name, tail):
        self.name, self.tail = name, tail

    def text(self):
        d = os.path.join(PLAY_SESSIONS, self.name)
        s = Script()
        s.items = [open(os.path.join(d, 'script.txt')).read().strip()]
        s.t = json.load(open(os.path.join(d, 'meta.json')))['end'] + 30
        self.tail(s)
        return s.text()


def end_run_card(s):
    """(routes2) A session left riding: the pause menu, END RUN (the cursor wraps up to it), EXIT? YES: RUN FINISHED, then
    card_tail."""
    s.press('START', after=120)
    s.press('UP', after=30)
    s.press('CROSS', after=120)
    s.press('DOWN', after=30)
    s.press('CROSS', after=900)
    card_tail(s)


ROUTES['se1-card'] = (lambda: PlayedTail('se1-card', end_run_card), 'routes2: played session se1-card (Woodward, 6,000 ticks), END RUN, the card: save game, save replay, the replay loaded and watched, load game')
ROUTES['se-card'] = (lambda: PlayedTail('p3-se0', card_tail), 'routes2: played session p3-se0 (Woodward), then the card: save game, save replay, the replay loaded and watched, load game')


def script_of(name):
    return ROUTES[name][0]().text()


def extra_env(name):
    """A route's own environment (None: unset)."""
    return ENVIRONMENT.get(name, {})


# Port 1 mirrors port 0: a second pad, which MULTIPLAYER asks for.
ENVIRONMENT.update({k: {'ZX_PAD_PORT1': 'mirror'} for k in ROUTES if k.startswith('mp-')})
ENVIRONMENT['saves'] = {'ZX_SAVES': None, 'ZX_MC_FD_BASE0': None}
ENVIRONMENT['se1-card'] = {'ZX_EE_SCALE': '4096', 'ZX_MC_FIXED_TIME': '1'}
if os.path.exists(os.path.join(PLAY_SESSIONS, 'p3-se0', 'meta.json')):
    ENVIRONMENT['se-card'] = dict({'ZX_EE_SCALE': '4096', 'ZX_MC_FIXED_TIME': '1'},
                                  **json.load(open(os.path.join(PLAY_SESSIONS, 'p3-se0', 'meta.json'))).get('env', {}))


def run(name, line):
    g = Game(name)
    for item in line.split(';'):
        w_ = item.split()
        if not w_:
            continue
        if w_[0] == 'raw':
            print(g.raw(' '.join(w_[1:])))
            continue
        if w_[0] not in COMMANDS:
            raise SystemExit('unknown command ' + w_[0])
        COMMANDS[w_[0]](g, *w_[1:])


def main():
    a = sys.argv[1:]
    if not a or a[0] == 'help':
        print(__doc__)
        for k, f in COMMANDS.items():
            print('  %-14s %s' % (k, f.__doc__))
    elif a[0] == 'routes':
        for k, v in ROUTES.items():
            print('%-16s %s' % (k, v[1]))
    elif a[0] == 'script':
        print(script_of(a[1]))
    elif a[0] == 'env':
        for k, v in extra_env(a[1]).items():
            print('%s=%s' % (k, '' if v is None else v))
    elif a[0] == 'start':
        start(a[1], a[2:])
    elif a[0] == 'stop':
        stop(a[1])
    else:
        run(a[0], ' '.join(a[1:]))


if __name__ == '__main__':
    main()
