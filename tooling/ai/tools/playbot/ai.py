#!/usr/bin/env python3
"""Aggressive Inline: the player's table (the game's state by name) and how to play.

    ai.py session NAME K LEVEL TICKS [SEED [SKATER]] [--real]   play level select entry K (nav.py's LEVEL) as a career
                                          (--real: a fresh card, nothing unlocked); NAME's pad script in sessions/
    ai.py replay NAME                     the script alone: the same game tick for tick?
    ai.py shots NAME [N]                  the session drawn, pictures at N key moments
    ai.py measure INPUT TICKS             what an input does: career, logic, an ab_inputs name or a file
    ai.py report NAME ...                 the game's numbers for sessions and inputs measured
    ai.py objectives [K ...]              each career level's objectives and givers, read from the game
    ai.py mp NAME GAME TICKS [SEED [ROUNDS]]  split screen (port 1 mirrors port 0): the Multiplayer menu's GAME
                                          (4 Animal Rescue) played to its results
    ai.py pe NAME TICKS [SEED [THEME]]    the Park Editor on a fresh card: a new park, towers, a Test Run skated
    ai.py reload NAME SAVES [PROFILE [K]] a native-file career (SAVES: its folder; PROFILE another) loaded: CAREER, entry
                                          K placed, the profile's open levels, goals and completion printed

Scratch: $PLAYAI (runs, sessions, measure, level). Runs stop at their tick limit, RunConfig's wall limit, or when
this player goes away.

The player is a generator: every `yield` is one pad answer. Behaviours are generators too, composed
with `yield from`, so a move reads as what a person does: hold, wait for the game to say so, let go.
Addresses come from readable_symbols.tsv by name; nothing here is a game address literal but the
Player's field offsets, which are game/aggressive/skater/skater.h's.
"""
import dataclasses
import gzip
import json
import math
import os
import random
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))
import zpaths  # noqa: E402  (tooling/paths.env)
MAIN = os.environ.get('AI_MAIN', zpaths.AI_WORKSPACE)
ZAXIS = zpaths.ZAXIS_MAIN
sys.path.insert(0, HERE)
import core  # noqa: E402
from core import pad  # noqa: E402
import nav  # noqa: E402

SCRATCH = os.environ.get('PLAYAI', os.path.join(zpaths.ZAXIS_SCRATCH, 'playai'))
RUNNER = os.environ.get('RUNNER', os.path.join(SCRATCH, 'bin', 'runner-play'))
ELF = os.environ.get('ELF', os.path.join(MAIN, 'recomp', 'disc-root', 'SLUS_203.27'))
VERBOSE = bool(os.environ.get('PLAY_VERBOSE'))
SYMBOLS = zpaths.tree_path(ZAXIS, 'runtime', 'tools', 'readable_symbols.tsv')

STATES = ('Ground Stand Air AirQPipe AirSpine Grind HandGrind DoubleGrind Stall VerticalPole VerticalPoleA '
          'HorizontalPole WallRide WallTap Manual CessSlide Vault Skitch').split()
GROUND, STAND, AIR, AIR_QPIPE, AIR_SPINE, GRIND, HAND_GRIND, DOUBLE_GRIND, STALL, VPOLE, VPOLE_A, HPOLE, WALL_RIDE, \
    WALL_TAP, MANUAL, CESS, VAULT, SKITCH = range(18)
AIRBORNE = (AIR, AIR_QPIPE, AIR_SPINE)
GRINDS = (GRIND, HAND_GRIND, DOUBLE_GRIND)

# GameMode (game/aggressive/globals.h)
FRONT_END, PLAY, PAUSED = 1, 2, 3
K_DOWN = 0x2             # Player::flags: bailed, the respawn timer runs
PANEL_CURSOR = 0x114     # a zgui panel's cursor (tools/sweep.py)
UNPLACED_Y = 20000.0     # the player's position before it is placed
PROFILE = 0x5E0          # Player: SkaterProfile (game/aggressive/career.h)
LEVEL_RECORD = 0xAC      # SkaterProfile: LevelRecord[14]
LEVEL_RECORD_SIZE = 0x24
OBJECTIVE_SIZE = 0x2C    # level/pause_parts.h Objective


def symbols():
    table = {}
    with open(SYMBOLS) as f:
        for line in f:
            w = line.rstrip('\n').split('\t')
            if len(w) >= 5 and w[0] == 'data' and w[4] == '':
                table[w[1]] = int(w[2], 16)
    return table


SYM = symbols()
G_MODE, G_PANEL, G_LEVEL, G_FRAME, G_TOP, G_CLOCK = (SYM[n] for n in (
    'g_gameMode', 'g_topPanel', 'g_levelNumber', 'g_levelFrame', 'g_topState', 'g_levelTime'))
G_OBJECTIVES, G_OBJECTIVE_COUNT = SYM['g_objectives'], SYM['g_objectiveCount']


@dataclasses.dataclass
class S:
    """What the player sees at one pad read."""
    tick: int = 0
    mode: int = 0
    panel: int = 0
    cursor: int = -1
    level: int = -1
    frame: int = 0
    clock: float = 0.0
    player: int = 0
    st: int = -1            # Player::state
    substate: int = 0
    landing: int = 0
    flags: int = 0
    fakie: int = 0
    pos: tuple = (0.0, 0.0, 0.0)
    vel: tuple = (0.0, 0.0, 0.0)
    forward: tuple = (0.0, 0.0, 1.0)
    speed: float = 0.0
    state_time: float = 0.0
    air_time: float = 0.0
    height: float = 0.0
    balance: float = 0.0
    manual_balance: float = 0.0
    manual_speed: float = 0.0
    drift: float = 0.0
    crash: int = 0
    juice: float = -1.0
    carton: float = 0.0
    pending: float = 0.0
    multiplier: int = 0
    score: int = 0
    points: int = 0
    skater: int = 0
    grind_rail: int = 0
    rail_a: tuple = (0.0, 0.0, 0.0)
    rail_b: tuple = (0.0, 0.0, 0.0)
    rail_dir: tuple = (0.0, 0.0, 1.0)
    spin: float = 0.0
    open_levels: int = 0
    goals: tuple = ()
    challenge_points: tuple = ()
    rewards: tuple = ()
    completion: float = 0.0
    digest: bytes = None

    @property
    def placed(self):
        return self.mode == PLAY and self.player != 0 and abs(self.pos[1]) < UNPLACED_Y - 1 and self.frame > 2

    @property
    def down(self):
        return bool(self.flags & K_DOWN)

    @property
    def air(self):
        return self.st in AIRBORNE

    @property
    def grinding(self):
        return self.st in GRINDS


def meter_cells(run, meter):
    """Where the juice carton keeps TangAmount and CartonSize: its Variable children (zgui Node)."""
    cells = {}
    child = run.u32(meter + 0x04)
    for _ in range(64):
        if not child:
            break
        name = text_at(run, run.u32(child + 0x14), 32)
        if name in ('TangAmount', 'CartonSize'):
            cells[name] = run.u32(child + 0x18)
        child = run.u32(child)
    return cells


METERS = {}


def read(run):
    s = S(tick=run.tick)
    g = run.mem
    s.mode = struct.unpack('<I', g(G_MODE, 4))[0]
    s.panel = struct.unpack('<I', g(G_PANEL, 4))[0]
    s.level = struct.unpack('<i', g(G_LEVEL, 4))[0]
    s.frame = struct.unpack('<I', g(G_FRAME, 4))[0]
    s.clock = struct.unpack('<f', g(G_CLOCK, 4))[0]
    if s.panel:
        s.cursor = run.u32(s.panel + PANEL_CURSOR)
    top = run.u32(G_TOP)
    players = run.u32(top + 0x10) if top else 0
    if not players:
        s.digest = struct.pack('<IIi', s.mode, s.panel, s.level)
        return s
    s.player = p = players + 0x10
    b = g(p, 0x930)
    fl = lambda o: struct.unpack_from('<f', b, o)[0]
    i32 = lambda o: struct.unpack_from('<i', b, o)[0]
    s.st, s.substate, s.landing, s.fakie = b[0x74], b[0x76], b[0x7C], b[0x02]
    s.flags = i32(0x80)
    s.pos = struct.unpack_from('<3f', b, 0x90)
    s.vel = struct.unpack_from('<3f', b, 0xA0)
    s.forward = struct.unpack_from('<3f', b, 0xD0)
    s.height, s.air_time, s.speed = fl(0x1C0), fl(0x1DC), fl(0x1E8)
    s.grind_rail = i32(0x214)
    s.rail_a = struct.unpack_from('<3f', b, 0x220)
    s.rail_b = struct.unpack_from('<3f', b, 0x230)
    s.rail_dir = struct.unpack_from('<3f', b, 0x240)
    s.spin = fl(0x1D4) + fl(0x1D8)
    s.balance, s.manual_balance, s.drift, s.manual_speed, s.state_time = fl(0x270), fl(0x274), fl(0x278), fl(0x27C), fl(0x29C)
    s.crash = i32(0x618)
    s.pending, s.multiplier, s.points = fl(0x8C8), i32(0x8CC), i32(0x8D8)
    score = struct.unpack_from('<I', b, 0x8C4)[0]
    if score:
        s.score = run.u32(score + 0x14)
    meter = struct.unpack_from('<I', b, 0x8BC)[0]
    if meter:
        cells = METERS.get(meter)
        if cells is None or len(cells) < 2:
            cells = METERS[meter] = meter_cells(run, meter)
        if 'TangAmount' in cells:
            s.juice = struct.unpack('<f', g(cells['TangAmount'], 4))[0]
        if 'CartonSize' in cells:
            s.carton = struct.unpack('<f', g(cells['CartonSize'], 4))[0]
    s.skater = b[PROFILE] & 0x1F
    s.open_levels = i32(PROFILE + 4)
    s.completion = fl(PROFILE + 0x14)
    rec = PROFILE + LEVEL_RECORD
    s.challenge_points = tuple(i32(rec + 0x00 + n * LEVEL_RECORD_SIZE) for n in range(14))
    s.goals = tuple(i32(rec + 0x04 + n * LEVEL_RECORD_SIZE) for n in range(14))
    s.rewards = tuple(i32(rec + 0x10 + n * LEVEL_RECORD_SIZE) for n in range(14))
    s.digest = b[0x74:0x80] + b[0x90:0xB0] + b[0xD0:0xE0] + b[0x1DC:0x1F8] + b[0x8C4:0x8D0] + struct.pack(
        '<IIIi', s.mode, s.panel, s.frame, s.level)
    return s


class Player:
    """Drives a behaviour generator: the policy core.play calls."""

    def __init__(self, run, behaviour, seed=1):
        self.run = run
        self.s = S()
        self.gen = behaviour(self)
        self.pad = core.NOTHING
        self.rng = random.Random(seed)
        self.notes = {}
        self.lip = 0.0

    def tally(self, what):
        self.notes[what] = self.notes.get(what, 0) + 1
        if VERBOSE:
            s = self.s
            print('%d %s | %.0f %.1f %.0f sp %.1f %s j %.1f score %d' % (s.tick, what, s.pos[0], s.pos[1], s.pos[2], s.speed,
                                                                         STATES[s.st] if 0 <= s.st < 18 else s.st, s.juice, s.score), flush=True)

    def __call__(self, s):
        if s.st in (AIR_QPIPE, AIR_SPINE) and self.s.st != s.st:
            self.lip = s.pos[1]
        self.s = s
        try:
            return next(self.gen)
        except StopIteration:
            return None


# ---- menus -----------------------------------------------------------------------------------

def wait(bot, n, p=core.NOTHING):
    for _ in range(n):
        yield p


def tap(bot, button, hold=6, gap=20):
    yield from wait(bot, hold, pad(button))
    yield from wait(bot, gap)


def until(bot, test, limit, p=core.NOTHING):
    """Hold p until test(state); False at the limit."""
    for _ in range(limit):
        if test(bot.s):
            return True
        yield p
    return False


def new_panel(bot, old, limit=1200):
    """A new zgui panel that stays for 40 reads."""
    last, held = None, 0
    for _ in range(limit):
        p = bot.s.panel
        held = held + 1 if p and p != old and p == last else 0
        last = p
        if held >= 40:
            return p
        yield core.NOTHING
    return bot.s.panel


def set_cursor(bot, k, inc='RIGHT', dec='LEFT', limit=20):
    for _ in range(limit):
        if bot.s.cursor == k:
            return True
        yield from tap(bot, inc if bot.s.cursor < k else dec, gap=25)
    return False


def boot(bot):
    """Past the intro, to the main menu."""
    yield from wait(bot, 400)
    for _ in range(3):
        yield from tap(bot, 'START', hold=10, gap=30)
    yield from until(bot, lambda s: s.mode == FRONT_END and s.panel != 0, 2400)
    yield from wait(bot, 90)


def close_boxes(bot, limit=30):
    n = 0
    while bot.s.mode == PAUSED and n < limit:
        yield from tap(bot, 'CROSS', hold=6, gap=40)
        n += 1


NAME_ENTRY_END = 38      # the name entry's keyboard cursor on END


def wait_placed(bot, limit=9000):
    yield from until(bot, lambda s: s.mode == PLAY, 3600)
    for _ in range(limit // 10):
        if bot.s.placed:
            break
        if bot.s.mode == PAUSED:
            yield from close_boxes(bot)
        yield from wait(bot, 10)
    yield from wait(bot, 30)
    yield from close_boxes(bot)


def career(bot, k, skater=0):
    """From the main menu: CAREER, skater `skater`, level select entry k."""
    p0 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=30)
    p1 = yield from new_panel(bot, p0)
    yield from set_cursor(bot, skater)
    yield from tap(bot, 'CROSS', gap=30)
    p2 = yield from new_panel(bot, p1)
    if bot.s.cursor == NAME_ENTRY_END:
        yield from tap(bot, 'CROSS', gap=30)
        yield from new_panel(bot, p2)
    yield from set_cursor(bot, k)
    yield from tap(bot, 'CROSS', gap=30)
    yield from wait_placed(bot)


def text_at(run, address, limit=200):
    data = run.mem(address, limit)
    return data.split(b'\0')[0].decode('latin1')


STATE_NAMES = ('Init', 'Active', 'Wait', 'Complete', 'WaitSuccess', 'WaitFail')


def objectives(run):
    """The level's Objective records (level/pause_parts.h), with their script strings."""
    records = run.u32(G_OBJECTIVES)
    count = run.u32(G_OBJECTIVE_COUNT)
    strings = run.u32(SYM['g_vm'])
    hashes = struct.unpack('<6I', run.mem(SYM['g_stateHashes'], 24))
    out = []
    for n in range(count):
        o = struct.unpack('<11I', run.mem(records + n * OBJECTIVE_SIZE, OBJECTIVE_SIZE))
        state = STATE_NAMES[hashes.index(o[3])] if o[3] in hashes else hex(o[3])
        out.append(dict(index=n, locked=o[0], kind=o[1], points=o[2], state=state, object=o[5], done=o[10],
                        title=text_at(run, strings + o[6]), text=text_at(run, strings + o[7], 300)))
    return out


def bottom_of_menu(bot):
    """The cursor down to the last entry of a pause menu."""
    last = None
    for _ in range(40):
        yield from tap(bot, 'DOWN', hold=6, gap=10)
        if bot.s.cursor == last:
            return
        last = bot.s.cursor


def to_level_select(bot):
    """Pause, LEVEL SELECT (the entry above EXIT); the front end's level select."""
    yield from close_boxes(bot)
    yield from tap(bot, 'START', gap=40)
    yield from bottom_of_menu(bot)
    yield from tap(bot, 'UP', gap=20)
    old = bot.s.panel
    yield from tap(bot, 'CROSS', gap=40)
    yield from until(bot, lambda s: s.mode == FRONT_END, 3600)
    yield from new_panel(bot, old)


def next_level(bot, k):
    """From the level select to entry k, placed."""
    yield from set_cursor(bot, k)
    yield from tap(bot, 'CROSS', gap=30)
    yield from wait_placed(bot)


# ---- skating ---------------------------------------------------------------------------------

def heading(s):
    if s.speed > 3.0:
        return math.atan2(s.vel[2], s.vel[0])
    return math.atan2(s.forward[2], s.forward[0])


def turn_to(s, target):
    """Radians to turn to face target; positive is the stick's left."""
    return core.wrap(math.atan2(target[2] - s.pos[2], target[0] - s.pos[0]) - heading(s))


def stick(v):
    """A stick value in steps, as a thumb gives: a recorded script stays small."""
    v = round(core.clamp(v, -1.0, 1.0) * 3.0) / 3.0
    return v if v else None


def steer(s, target, *buttons, gain=2.4):
    return pad('UP', *buttons, lx=stick(-turn_to(s, target) * gain))


def flat_dist(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


class Plan:
    """What the player knows of the level, and what has worked."""

    def __init__(self, level_name):
        self.lv = nav.Level(level_name)
        self.entries = None
        self.tried = {}

    def rail_entries(self, part, keen=False):
        """Ways onto rails from this part of the level: a run-up along the rail's line, the rail in jumping reach
        above it. keen: for a rail wanted by name, a short run-up that may slope."""
        lv, out = self.lv, []
        backs = (2.0, 4.0, 6.0, 8.0) if keen else (2.0, 5.0, 8.0, 11.0, 14.0)
        for n, r in enumerate(lv.rails):
            if r['length'] < 5.0:
                continue
            pts = r['points']
            for ends in (pts, pts[::-1]):
                a, b = ends[0], ends[1]
                seg = math.dist(a, b)
                if seg < 2.0:
                    continue
                d = tuple((q - p) / seg for p, q in zip(a, b))
                if abs(d[1]) > 0.5:
                    continue
                t = tuple(p + v * min(2.5, seg / 2) for p, v in zip(a, d))
                run_up = []
                for back in backs:
                    q = (t[0] - d[0] * back, t[1] + 2.0, t[2] - d[2] * back)
                    node = lv.nearest(q, 2 if keen else 1)
                    if node is None or lv.part.get(node) != part:
                        break
                    run_up.append(lv.world(node))
                if len(run_up) < len(backs):
                    continue
                under = [q[1] - t[1] for q in run_up]
                # +y is down: the rail 0 to 3 above the run-up, the run-up level
                spread = max(q[1] for q in run_up) - min(q[1] for q in run_up)
                if not (-0.2 <= under[0] <= (3.4 if keen else 3.0)) or spread > (4.0 if keen else 1.5):
                    continue
                out.append(dict(rail=n, start=run_up[-1], on=t, dir=d, length=r['length'], kind=r['kind'], name=r['name']))
        return out

    def close_near(self, pos):
        """The joined gaps near a place where the way turned out to be shut."""
        lv = self.lv
        for node in [n for n in lv.extra if flat_dist(lv.world(n), pos) < 12.0]:
            del lv.extra[node]
        for node, ways in lv.extra.items():
            lv.extra[node] = [w for w in ways if (w[0], w[1]) in lv.extra or flat_dist(lv.world((w[0], w[1])), pos) >= 12.0]

    def open_cells(self):
        """Points of the level's largest group of cells with ways back and forth."""
        if getattr(self, 'open_list', None) is None:
            group, sizes = self.lv.strong()
            big = max(range(len(sizes)), key=lambda n: sizes[n])
            self.open_list = [self.lv.world(c) for c, g in group.items() if g == big][::3]
        return self.open_list

    def cells_of(self, part):
        cached = getattr(self, 'cell_list', None)
        if cached is None or cached[0] != part:
            self.cell_list = cached = (part, [k for k, p in self.lv.part.items() if p == part])
        return cached[1]

    def doubles(self):
        """Whether the level has double rails (edge kinds 5 and 6)."""
        if getattr(self, '_doubles', None) is None:
            self._doubles = any(r['kind'] in DOUBLE_KINDS for r in self.lv.rails)
        return self._doubles

    def between(self, e):
        """A double rail's entry moved to the middle of its pair: the nearest rail of the other kind (5 and 6), parallel
        and within 2.5 to the side; None without one."""
        on, d = e['on'], e['dir']
        best = None
        for r in self.lv.rails:
            if r['kind'] == e['kind'] or r['kind'] not in (5, 6):
                continue
            dh = math.hypot(d[0], d[2])
            for a, b in zip(r['points'], r['points'][1:]):
                seg = math.hypot(b[0] - a[0], b[2] - a[2])          # along the ground: the rails may slope
                if seg < 0.5:
                    continue
                u = ((b[0] - a[0]) / seg, (b[2] - a[2]) / seg)
                if abs(u[0] * d[0] + u[1] * d[2]) / dh < 0.97:
                    continue
                t = (on[0] - a[0]) * u[0] + (on[2] - a[2]) * u[1]
                if not 0.0 <= t <= seg:
                    continue
                q = (a[0] + u[0] * t, a[1] + (b[1] - a[1]) * t / seg, a[2] + u[1] * t)
                side = math.hypot(q[0] - on[0], q[2] - on[2])
                if side < 2.5 and abs(q[1] - on[1]) < 0.6 and (best is None or side < best[0]):
                    best = (side, q)
        if best is None:
            return None
        q = best[1]
        dx, dz = (q[0] - on[0]) / 2, (q[2] - on[2]) / 2
        return dict(e, on=(on[0] + dx, on[1], on[2] + dz), start=(e['start'][0] + dx, e['start'][1], e['start'][2] + dz))

    def next_rail(self, s, rng, only=None, kinds=None):
        part = self.lv.main_part(s.pos)
        if self.entries is None or self.entries[0] != part:
            self.entries = (part, self.rail_entries(part))
        best, score = None, -1e18
        entries = self.entries[1]
        if only:
            entries = [e for e in self.rail_entries(part, keen=True) if re.search(only, e['name'], re.I)]
        if kinds:
            entries = [self.between(e) for e in self.rail_entries(part, keen=True) if e['kind'] in kinds]
            entries = [e for e in entries if e]
        for e in entries:
            d = flat_dist(s.pos, e['start'])
            tries, got = self.tried.get(e['rail'], (0, 0))
            v = min(e['length'], 40.0) * 0.8 - d * 1.5 - tries * 60.0 + rng.random() * 25.0
            if d > 6.0 and v > score:
                best, score = e, v
        return best


def bailed(s):
    return s.down or s.mode != PLAY


def recover(bot):
    """After a bail: boxes closed, the skater up again."""
    n = 0
    while (bot.s.mode != PLAY or bot.s.down) and n < 600:
        if bot.s.mode == PAUSED:
            yield from close_boxes(bot)
        yield core.NOTHING
        n += 1


def follow(bot, path, near=3.0, limit=1800, plan=None):
    """Skate a path of points; False if stuck, bailed or out of time. With plan, lines of hops on the way."""
    if not path:                # (a giver beside the skater: path_beside's empty way) nowhere to go
        return False
    i, slow, calm = 0, 0, 0
    for _ in range(limit):
        s = bot.s
        if s.st == MANUAL:
            yield from wait(bot, 3, pad('CROSS'))
            yield from wait(bot, 12)
            continue
        calm = calm + 1 if s.speed > 9.0 and s.st == GROUND else 0
        if plan and calm > 50 and abs(turn_to(s, path[i])) < 0.6 and clear_ahead(plan, s):
            calm = 0
            yield from hops(bot, plan, path[i:])
            continue
        if bailed(s):
            return False
        while i < len(path) - 1 and flat_dist(s.pos, path[i]) < max(4.0, s.speed * 0.5):
            i += 1
        if i == len(path) - 1 and flat_dist(s.pos, path[-1]) < near:
            return True
        slow = slow + 1 if s.speed < 1.5 else 0
        if slow > 90:
            bot.tally('stuck')
            return False
        yield steer(s, path[i])
    return False


def clear_ahead(plan, s):
    """Nothing known against a hop: floor where it lands is near this height, or the grid has no cell there."""
    x = s.pos[0] + s.vel[0] * 0.75
    z = s.pos[2] + s.vel[2] * 0.75
    node = plan.lv.nearest((x, s.pos[1], z), 1)
    return node is None or -1.5 < plan.lv.world(node)[1] - s.pos[1] < 3.0


def unstick(bot):
    """Back off whatever stopped the skater, a turn and an ollie."""
    side = 1.0 if bot.rng.random() < 0.5 else -1.0
    yield from wait(bot, 25, pad('DOWN', lx=side))
    yield from wait(bot, 25, pad('UP', lx=side))
    yield from wait(bot, 4, pad('UP', 'CROSS'))
    yield from wait(bot, 25, pad('UP'))


def balance_stick(s):
    """Against the balance and its drift (grindBalance: drift += stick; balance += drift)."""
    return core.clamp(-(s.balance * 3.0 + s.drift * 40.0), -1.0, 1.0)


def manual_stick(s):
    """As the grind's: manualBalance drifts away from the middle, stickY pushes back."""
    return core.clamp(-(s.manual_balance * 3.0 + s.manual_speed * 40.0), -1.0, 1.0)


def ahead_on(path, s, reach=14.0):
    """The path's point about `reach` ahead of the skater."""
    best = min(range(len(path)), key=lambda n: flat_dist(s.pos, path[n]))
    for n in range(best, len(path)):
        if flat_dist(s.pos, path[n]) >= reach:
            return path[n]
    return path[-1]


def far_path(plan, s, rng, least=80.0):
    """A long way over this part of the level from where the skater is."""
    lv, part = plan.lv, plan.lv.main_part(s)
    if part is None:
        return None
    cells = plan.cells_of(part)
    for _ in range(8):
        goal = lv.world(rng.choice(cells))
        if flat_dist(goal, s) > least:
            path = lv.path(s, goal)
            if path:
                return path
    return None


def hops(bot, plan, path, most=8, extend=False, bank=None, keep=False):
    """A line along the path: ollie, trick, land in a manual, ollie again. A line is worth its points times its
    tricks, so it runs as long as the way is clear. A manual is only ever left by the next ollie: held UP would push
    its balance over, and that bails. The last hop lands plainly and banks. extend: at the path's end, on to another."""
    for n in range(most):
        s = bot.s
        if extend and flat_dist(s.pos, path[-1]) < max(20.0, s.speed * 2.0):
            more = far_path(plan, path[-1], bot.rng, 60.0)
            if more:
                path = path[-30:] + more
        target = ahead_on(path, s)
        last = n == most - 1 or flat_dist(s.pos, path[-1]) < s.speed * 1.5 or abs(turn_to(s, target)) > (1.2 if extend else 0.8) or not clear_ahead(plan, s)
        last = last or (bank is not None and s.pending * s.multiplier >= bank)
        if keep:                       # the line goes on past this path: stop in the manual, at its end
            if flat_dist(s.pos, path[-1]) < max(6.0, s.speed * 0.9):
                return
            last = False
        if bailed(s) or (n == 0 and last and s.st != MANUAL):
            return
        yield from wait(bot, 3, pad('CROSS', lx=stick(-turn_to(s, target) * 2.0)))
        yield from until(bot, lambda q: q.air, 12)      # a manual's jump leaves the ground some frames after the button
        if not bot.s.air:
            return
        bot.tally('hop')
        yield from air_trick(bot, plan, least=0.3, stop=0.26, manual=not last)
        if last or bot.s.st != MANUAL:
            return
        bot.tally('manual')
        for _ in range(14 + bot.rng.randrange(14)):
            s = bot.s
            if s.st != MANUAL:
                return
            yield pad(lx=stick(-turn_to(s, ahead_on(path, s)) * 2.0), ly=stick(manual_stick(s)))


def escape(bot, plan):
    """In a pocket with no way on by rolling (a bowl, a ring round a fountain): at the nearest open ground, hopping."""
    bot.tally('escape')
    cells = plan.open_cells()
    s = bot.s
    near = min(cells, key=lambda c: flat_dist(c, s.pos) if flat_dist(c, s.pos) > 6.0 else 1e9)
    for n in range(300):
        s = bot.s
        if bailed(s):
            return
        if n % 50 == 40:
            yield from wait(bot, 3, steer(s, near, 'CROSS'))
            if far_path(plan, bot.s.pos, bot.rng) is not None:
                return
        yield steer(s, near)


def long_line(bot, plan, most=36, bank=None):
    """Speed along a long way, then the longest line of hops and manuals it will take: score is points times tricks."""
    path = far_path(plan, bot.s.pos, bot.rng)
    if path is None:
        yield from escape(bot, plan)
        return False
    slow = 0
    for n in range(500):
        s = bot.s
        if bailed(s):
            return False
        target = ahead_on(path, s)
        if s.st == MANUAL:
            break                      # still in a manual: the line goes on from here
        fast = 14.0 if n < 250 else 8.0            # a tight level never gives the long run-up: go with what there is
        if n > 8 and s.st == GROUND and s.speed > fast and abs(turn_to(s, target)) < 0.5 and clear_ahead(plan, s):
            break
        if flat_dist(s.pos, path[-1]) < 8.0:
            path = far_path(plan, s.pos, bot.rng)
            if path is None:
                return False
        slow = slow + 1 if s.speed < 2.0 else 0
        if slow > 60:
            bot.tally('line: stuck')
            return False
        yield steer(s, target)
    else:
        bot.tally('line: no run-up')
        return False
    bot.tally('line start')
    yield from hops(bot, plan, path, most=most, extend=True, bank=bank)
    yield from wait(bot, 2)
    s = bot.s
    bot.tally('line end x%d pend %.0f score %d' % (s.multiplier, s.pending, s.score))
    return True


def ride_rail(bot, plan, chain=False):
    """On a rail: balance held, grind tricks along it, an ollie off its end, a trick in that. chain: into a manual and
    no further, the next rail is the line's next trick."""
    n, next_trick, last = 0, 30, None
    while bot.s.grinding and n < 1800:
        s = bot.s
        n += 1
        if s.st == DOUBLE_GRIND and last != DOUBLE_GRIND:
            bot.tally('double grind')
        last = s.st
        lean = stick(balance_stick(s))
        if s.speed > 2.0 and rail_left(s) < max(1.5, s.speed * 0.14) and n > 12:
            yield from wait(bot, 3, pad('CROSS'))
            yield from wait(bot, 2)
            if chain:                      # the next rail in line, if there is one this hop reaches
                for _ in range(32):
                    if not bot.s.air:
                        break
                    yield pad('TRIANGLE')
                if bot.s.grinding:
                    return
            if bot.s.air:
                bot.tally('off the rail')
                yield from air_trick(bot, plan, least=0.35, stop=0.24, manual=True)
                if bot.s.st == MANUAL and not chain:
                    s = bot.s
                    yield from hops(bot, plan, [(s.pos[0] + s.vel[0] * t, s.pos[1], s.pos[2] + s.vel[2] * t) for t in (0.5, 1.0, 2.0, 3.0)])
            return
        if n >= next_trick:
            next_trick = n + 45
            tap_ = bot.rng.choice(('LEFT', 'RIGHT', 'UP', 'DOWN'))
            bot.tally('grind trick')
            yield from wait(bot, 2, pad(tap_))
            yield from wait(bot, 3, pad('TRIANGLE'))
            continue
        yield pad(lx=lean)


def onto_rail(bot, plan, e):
    """From along the rail's line, on the ground or in a manual: the hop, the grind held. True when on it."""
    on, d = e['on'], e['dir']
    for _ in range(300):
        s = bot.s
        if bailed(s) or (s.st != MANUAL and not (s.st in (GROUND, STAND)) and not s.air):
            return False
        left = (on[0] - s.pos[0]) * d[0] + (on[2] - s.pos[2]) * d[2]
        if left < max(3.0, s.speed * 0.38):
            break
        aim = (on[0] + d[0] * 2, on[1], on[2] + d[2] * 2)
        if s.st == MANUAL:
            yield pad(lx=stick(-turn_to(s, aim) * 2.4), ly=stick(manual_stick(s)))
        else:
            yield steer(s, aim)
    else:
        return False
    was_manual = bot.s.st == MANUAL
    yield from wait(bot, 3, pad('CROSS') if was_manual else pad('UP', 'CROSS'))
    for n in range(90):
        s = bot.s
        if s.grinding:
            return True
        if bailed(s) or (not s.air and n > 20):
            return False
        aim = (on[0] + d[0] * 6, on[1], on[2] + d[2] * 6)
        yield pad('TRIANGLE', lx=stick(-turn_to(s, aim) * 2.0)) if was_manual else pad('UP', 'TRIANGLE', lx=stick(-turn_to(s, aim) * 2.0))
    return False


def in_a_row(e, entries):
    """How many differently named rails lie ahead along this one's line."""
    names = set()
    for q in entries:
        rx, rz = q['on'][0] - e['on'][0], q['on'][2] - e['on'][2]
        along, side = rx * e['dir'][0] + rz * e['dir'][2], rz * e['dir'][0] - rx * e['dir'][2]
        if 2.0 < along < 200.0 and abs(side) < 10.0 and q['dir'][0] * e['dir'][0] + q['dir'][2] * e['dir'][2] > 0.8:
            names.add(q['name'])
    return len(names)


def ahead_of(s, q):
    d = math.hypot(s.vel[0], s.vel[2]) or 1.0
    rx, rz = q['on'][0] - s.pos[0], q['on'][2] - s.pos[2]
    return (rx * s.vel[0] + rz * s.vel[2]) / d > 3.0 and (q['dir'][0] * s.vel[0] + q['dir'][2] * s.vel[2]) / d > 0.5


def rail_chain(bot, plan, pattern, want):
    """`want` rails of a name in one line: a manual and hops between them, the next rail taken from the manual."""
    part = plan.lv.main_part(bot.s.pos)
    entries = [e for e in plan.rail_entries(part, keen=True) if not pattern or re.search(pattern, e['name'], re.I)]
    got, used = 0, set()
    bot.tally('chain %s: %d ways' % (pattern, len(entries)))
    while got < want:
        s = bot.s
        left = [e for e in entries if e['name'] not in used and flat_dist(s.pos, e['on']) > 6.0]
        if not left:
            break
        if got == 0:
            e = max(left, key=lambda q: in_a_row(q, entries) * 100.0 - flat_dist(s.pos, q['start']) * 0.5 + bot.rng.random() * 40.0)
        else:
            e = min(left, key=lambda q: flat_dist(s.pos, q['start']) + (0.0 if ahead_of(s, q) else 400.0))
        used.add(e['name'])
        path = plan.lv.path(s.pos, e['start'])
        if path is None:
            continue
        if got == 0:
            if not (yield from follow(bot, path + [e['start']], near=4.0)):
                return got
        else:
            if bot.s.st != MANUAL:
                break
            yield from hops(bot, plan, path + [e['start']], most=12, keep=True)
            if bot.s.st != MANUAL:
                break
        if not (yield from onto_rail(bot, plan, e)):
            bot.tally('chain: missed %s after %d' % (e['name'], got))
            break
        got += 1
        bot.tally('chain %d: %s x%d' % (got, e['name'], bot.s.multiplier))
        yield from ride_rail(bot, plan, chain=True)
        while bot.s.grinding and got < want + 2:
            near = min(entries, key=lambda q: flat_dist(bot.s.pos, q['on']))
            got += 0 if near['name'] in used else 1
            used.add(near['name'])
            bot.tally('chain %d: on to %s x%d' % (got, near['name'], bot.s.multiplier))
            yield from ride_rail(bot, plan, chain=True)
    bot.tally('chain over %d of %d' % (got, want))
    return got


# Double rails (two parallel edges tagged 5 and 6: Boardwalk's coaster tracks and wharf rail, the Museum's beam): the
# d-pad UP three times, then the grind button, is a Double Grind (routes.py double_grind: the button table's entry 0).
DOUBLE_KINDS = (5, 6)


def rail_line(bot, plan, only=None, kinds=None, taps=()):
    """To a rail, along its line, up onto it, to its end. only: a pattern of rail names; kinds: the rails' edge kinds;
    taps: d-pad taps in the ollie before the grind button (UP x3: a Double Grind on double rails)."""
    e = plan.next_rail(bot.s, bot.rng, only, kinds)
    if e is None:
        return False
    bot.tally('goal rail %s at %.0f %.0f' % (e['name'], e['on'][0], e['on'][2]))
    tries, got = plan.tried.get(e['rail'], (0, 0))
    plan.tried[e['rail']] = (tries + 1, got)
    path = plan.lv.path(bot.s.pos, e['start'])
    if path is None:
        bot.tally('no path')
        return False
    if not (yield from follow(bot, path + [e['start']], near=4.0, plan=plan)):
        bot.tally('rail: not there')
        return False
    on, d = e['on'], e['dir']
    for _ in range(300):               # along the line, at speed
        s = bot.s
        if bailed(s):
            return False
        left = (on[0] - s.pos[0]) * d[0] + (on[2] - s.pos[2]) * d[2]
        if left < max(3.0, s.speed * 0.38):
            break
        side = (on[0] - s.pos[0]) * -d[2] + (on[2] - s.pos[2]) * d[0]
        aim = (on[0] + d[0] * 2, on[1], on[2] + d[2] * 2)
        yield steer(s, aim)
    else:
        return False
    yield from wait(bot, 3, pad('UP', 'CROSS'))
    for t in taps:
        yield from wait(bot, 2, pad(t))
        yield from wait(bot, 2)
    for _ in range(90):
        s = bot.s
        if s.grinding:
            plan.tried[e['rail']] = (tries, got + 1)
            bot.tally('rail ' + e['name'] + (' (%s)' % STATES[s.st] if taps else ''))
            yield from ride_rail(bot, plan)
            return True
        if bailed(s):
            bot.tally('rail: bailed')
            return False
        if not s.air and _ > 20:
            bot.tally('rail: missed')
            return True
        aim = (on[0] + d[0] * 6, on[1], on[2] + d[2] * 6)
        yield pad('UP', 'TRIANGLE', lx=stick(-turn_to(s, aim) * 2.0))
    return False


GRAVITY = 65.0           # units a second squared in plain Air, measured (+y is down)
QPIPE_GRAVITY = 40.0     # Physics / QPipe Gravity
# Entries of the air trick table (d-pad taps, then the button: routes.py AIR_TRICKS). A grab is over when let
# go; a flip must end its animation or the landing is "Greed!" (state_tricks.cpp AreYouFinished).
GRABS = (('LEFT',), ('RIGHT',), ('UP',), ('DOWN',))
FLIPS = (('LEFT', 'LEFT'), ('RIGHT', 'RIGHT'), ('UP', 'UP'), ('DOWN', 'DOWN'), ('LEFT', 'RIGHT'), ('RIGHT', 'LEFT'),
         ('UP', 'DOWN'), ('DOWN', 'UP'))


# PLAY_TRICKS=1: in an air long enough for a flip, the air table's tricks in turn instead of a random one: each grab
# direction with each flip's two (a combo: Rocket D, Stale R, Abstract U, Method L with FrontFlip U-U, BackFlip D-D,
# HotFlip L-L, the Flatspins R-L and L-R, Misty R-R, McTwist U-D, Bio D-U; proof-game's play goals), the grab's tap
# first and last (both orders, the table's history is matched newest first), and the two diagonal grabs (the stick
# down-right: Look Back Judo, down-left: Parallel That). A tap is the buttons pressed together.
PLAY_TRICKS = os.environ.get('PLAY_TRICKS') == '1'
TRICK_TURNS = ([((g,), f[0:1], f[1:2]) for g in ('DOWN', 'RIGHT', 'UP', 'LEFT') for f in FLIPS] +
               [(f[0:1], f[1:2], (g,)) for g in ('DOWN', 'RIGHT', 'UP', 'LEFT') for f in FLIPS])
DIAGONALS = ((('DOWN', 'RIGHT'),), (('DOWN', 'LEFT'),))


def next_trick(bot, left):
    """PLAY_TRICKS' next taps for an air with `left` seconds: a combo in a long air, else a diagonal grab."""
    if left > 1.1:
        n = getattr(bot, 'trick_turn', 0)
        bot.trick_turn = n + 1
        return TRICK_TURNS[n % len(TRICK_TURNS)], True
    n = getattr(bot, 'diagonal_turn', 0)
    bot.diagonal_turn = n + 1
    return DIAGONALS[n % 2], False


STALL_TAPS = (None, 'UP', 'DOWN', 'LEFT', 'RIGHT')     # the stall table: Hand Plant (no tap), then by direction


def plant_held(bot):
    """A hand plant trick held until its animation is over (stallTrickOnAnimationEnded): the stall table's direction
    in turn, TRIANGLE to the stall, then balance held against the stall's drift for up to three seconds."""
    n = getattr(bot, 'stall_turn', 0)
    bot.stall_turn = n + 1
    tap_ = STALL_TAPS[n % len(STALL_TAPS)]
    if tap_:
        yield from wait(bot, 2, pad(tap_))
        yield from wait(bot, 2)
    for _ in range(60):
        if bot.s.st == STALL:
            break
        yield pad('TRIANGLE')
    if bot.s.st != STALL:
        return
    bot.tally('stall held')
    for n in range(180):
        if bot.s.st != STALL:
            break
        yield pad(ly=stick(manual_stick(bot.s)))          # the stall's balance is balance[1], as the manual's
    bot.tally('stall %s held %d' % (tap_ or 'plant', n))


def double_line(bot, plan):
    """A Double Grind: a double rail, UP three times in the ollie, the grind button."""
    return (yield from rail_line(bot, plan, kinds=DOUBLE_KINDS, taps=('UP', 'UP', 'UP')))


def air_left(bot, plan, s):
    """Seconds to the landing: back to the lip in a pipe, else onto the floor under the landing point."""
    if s.st in (AIR_QPIPE, AIR_SPINE):
        drop, g = max(0.0, bot.lip - s.pos[1]), QPIPE_GRAVITY
        d = s.vel[1] * s.vel[1] + 2 * g * drop
        return (-s.vel[1] + math.sqrt(d)) / g if d > 0 else 0.0
    t = 0.4
    for _ in range(3):
        land = (s.pos[0] + s.vel[0] * t, s.pos[1], s.pos[2] + s.vel[2] * t)
        node = plan.lv.nearest(land, 2)
        floor = plan.lv.world(node)[1] if node else s.pos[1] + 2.0
        d = s.vel[1] * s.vel[1] + 2 * GRAVITY * (floor - s.pos[1])
        t = (-s.vel[1] + math.sqrt(d)) / GRAVITY if d > 0 else 0.0
    return t


def aligned(s):
    """How well the skater faces along its motion, either way: the landing's test (state_enters.cpp: under
    Landing Angle 0.3 a bail, over 0.9 clean, backwards lands fakie)."""
    size = math.hypot(s.vel[0], s.vel[2])
    if size < 1.0:
        return 1.0
    return abs(s.forward[0] * s.vel[0] + s.forward[2] * s.vel[2]) / (size * max(1e-6, math.hypot(s.forward[0], s.forward[2])))


def rail_left(s):
    """Units to the end of this rail segment, the way the skater heads."""
    ends = ((s.rail_a[i] - s.pos[i]) * s.rail_dir[i] for i in range(3)), ((s.rail_b[i] - s.pos[i]) * s.rail_dir[i] for i in range(3))
    return max(sum(e) for e in ends)


def air_trick(bot, plan, least=0.32, stop=0.2, manual=False):
    """A spin to fit the air, tricks for as long as it lasts, everything let go before the landing; with manual
    the taps of a manual just before it, to keep the line going."""
    left = air_left(bot, plan, bot.s)
    turns = 2 if left > 1.7 else 1 if left > 0.95 else 0
    goal = 360.0 * turns
    side = 'R1' if bot.rng.random() < 0.5 else 'L1'
    spinning = lambda: goal > 0 and (bot.s.spin < goal - 40.0 or aligned(bot.s) < 0.93)
    held = lambda: (side,) if spinning() else ()
    if goal:
        bot.tally('spin %d' % goal)
    start = bot.s.tick
    while bot.s.air and not bot.s.down:
        left = air_left(bot, plan, bot.s)
        if left < least:
            break
        if bot.s.tick - start > AIR_TOO_LONG:
            yield from out_of_the_air(bot)
            return
        flip = left > 1.1 and bot.rng.random() < 0.5
        taps = bot.rng.choice(FLIPS if flip else GRABS)
        button = 'SQUARE' if bot.rng.random() < 0.7 else 'CIRCLE'
        if PLAY_TRICKS and (left > 1.1 or bot.rng.random() < 0.3):
            taps, flip = next_trick(bot, left)
            bot.tally('trick %s %s' % ('-'.join('+'.join(t) for t in taps), button))
        for t in taps:
            t = t if isinstance(t, tuple) else (t,)
            yield from wait(bot, 2, pad(*t, *held()))
            yield from wait(bot, 2, pad(*held()))
        bot.tally('flip' if flip else 'grab')
        yield from wait(bot, 6, pad(button, *held()))
        if flip:
            for _ in range(45):        # its animation
                if not bot.s.air:
                    break
                yield pad(*held())
            continue
        for _ in range(240):
            if not bot.s.air or air_left(bot, plan, bot.s) < (0.5 if bot.s.st != AIR else stop):
                break
            yield pad(button, *held())
        yield from wait(bot, 3, pad(*held()))
    tapped = False
    while bot.s.air and not bot.s.down:
        if bot.s.tick - start > AIR_TOO_LONG:
            yield from out_of_the_air(bot)
            return
        if manual and not tapped and air_left(bot, plan, bot.s) < 0.24:
            tapped = True
            for t in ('UP', 'DOWN', 'UP'):
                yield from wait(bot, 2, pad(t))
                yield from wait(bot, 2)
            continue
        yield pad(*held())


def ramps(plan, part):
    """Rising runs of sloped cells with a run-up: (run-up start, ramp foot, top, direction)."""
    lv, out = plan.lv, []
    for key, ls in lv.nodes.items():
        for layer, (y, flat) in enumerate(ls):
            if flat > 0.97 or lv.part.get((key, layer)) != part:
                continue
            # a foot: a flat neighbour below, the cells rising away from it
            for di, dk in nav.NEAR[:4]:
                below = lv.nodes.get((key[0] - di, key[1] - dk))
                if not below or below[0][1] < 0.97:
                    continue
                n, top, yy, k = 0, None, y, key
                while n < 12:
                    nxt = lv.nodes.get((k[0] + di, k[1] + dk))
                    step = None
                    for oy, _ in nxt or ():
                        if -nav.STEP <= oy - yy < -0.1:      # +y is down: rising
                            step = oy
                    if step is None:
                        break
                    k, yy, n = (k[0] + di, k[1] + dk), step, n + 1
                if n < 2:
                    continue
                run = [(key[0] - di * m, key[1] - dk * m) for m in range(1, 8)]
                if not all(c in lv.nodes and lv.nodes[c][0][1] >= 0.9 and abs(lv.nodes[c][0][0] - below[0][0]) < 1.0 for c in run):
                    continue
                x, z = nav.centre(*key)
                sx, sz = nav.centre(*run[-1])
                tx, tz = nav.centre(*k)
                out.append(dict(start=(sx, below[0][0], sz), foot=(x, y, z), top=(tx, yy, tz), rise=y - yy))
    return out


PIPE_NAMES = ('qpipe', 'spine', 'halfpipe', 'bowl')


def pipes(plan, part):
    """Quarter pipes by their lips (rails of instances named so): the floor in front, a run at the lip."""
    lv, out = plan.lv, []
    for r in lv.rails:
        if not any(w in r['name'].lower() for w in PIPE_NAMES) or r['length'] < 3.0:
            continue
        pts = r['points']
        a, b = pts[0], pts[-1]
        size = math.dist(a, b)
        if size < 2.0:
            continue
        m = tuple((p + q) / 2 for p, q in zip(a, b))
        nx, nz = -(b[2] - a[2]) / size, (b[0] - a[0]) / size
        for side in (1.0, -1.0):
            near = lv.nearest((m[0] + nx * side * 9, m[1] + 4, m[2] + nz * side * 9), 2)
            far = lv.nearest((m[0] + nx * side * 18, m[1] + 4, m[2] + nz * side * 18), 2)
            if near is None or far is None or lv.part.get(near) != part or lv.part.get(far) != part:
                continue
            fy = lv.world(near)[1]
            if not 1.5 <= fy - m[1] <= 12.0:
                continue
            start = lv.world(far)
            out.append(dict(start=start, foot=lv.world(near), top=(m[0], m[1], m[2]), rise=fy - m[1], name=r['name']))
    return out


def next_ramp(plan, s, rng):
    part = plan.lv.main_part(s.pos)
    if getattr(plan, 'ramp_list', None) is None or plan.ramp_list[0] != part:
        plan.ramp_list = (part, pipes(plan, part) + ramps(plan, part))
        print('launches', len(plan.ramp_list[1]), 'rails', len(plan.rail_entries(part)), flush=True)
    best, score = None, -1e18
    for r in plan.ramp_list[1]:
        d = flat_dist(s.pos, r['start'])
        v = r['rise'] * 4 - d * 1.5 + rng.random() * 30.0 - plan.tried.get(('ramp', r['foot']), 0) * 60.0
        if d > 8.0 and v > score:
            best, score = r, v
    return best


def wall_entries(plan, part):
    """Tall walls with room: a run at them at about 45 degrees from a floor at their foot."""
    lv, out = plan.lv, []
    for x0, z0, x1, z1, top, bottom, nx, nz in lv.walls:
        tx, tz = (x1 - x0), (z1 - z0)
        size = math.hypot(tx, tz)
        tx, tz = tx / size, tz / size
        mx, mz = (x0 + x1) / 2, (z0 + z1) / 2
        for side in (1.0, -1.0):
            for along in (1.0, -1.0):
                at = lambda d: (mx + nx * side * d - tx * along * d, bottom, mz + nz * side * d - tz * along * d)
                near, far = lv.nearest(at(3.0), 2), lv.nearest(at(12.0), 2)
                if near is None or far is None or lv.part.get(near) != part or lv.part.get(far) != part:
                    continue
                floor = lv.world(near)[1]
                # +y is down: the wall stands at least 3 above this floor, its foot at or under it
                if floor - top < 3.0 or bottom < floor - 1.0 or abs(lv.world(far)[1] - floor) > 1.0:
                    continue
                out.append(dict(start=lv.world(far), wall=(mx, floor, mz), normal=(nx * side, nz * side)))
    return out


WALL_LET_GO = os.environ.get('PLAY_WALL_LET_GO') == '1'


def wall_line(bot, plan, centre=None, radius=40.0, hold=50):
    """Jump at a wall at an angle, Down and Cross to take it (routes.py wallride), a trick off the end. With centre,
    one of the walls within radius of it (an objective's wall near its giver)."""
    part = plan.lv.main_part(bot.s.pos)
    if getattr(plan, 'wall_list', None) is None or plan.wall_list[0] != part:
        plan.wall_list = (part, wall_entries(plan, part))
    if centre is None:
        near = [w for w in plan.wall_list[1] if 8.0 < flat_dist(bot.s.pos, w['start']) < 90.0]
    else:
        near = [w for w in plan.wall_list[1] if flat_dist(centre, w['wall']) < radius and flat_dist(bot.s.pos, w['start']) > 4.0]
    if not near:
        return False
    w, path = None, None
    for c in sorted(bot.rng.sample(near, min(8, len(near))), key=lambda e: flat_dist(bot.s.pos, e['start'])):
        path = plan.lv.path(bot.s.pos, c['start'])
        if path:
            w = c
            break
    if w is None:
        return False
    bot.tally('goal wall at %.0f %.0f' % (w['wall'][0], w['wall'][2]))
    if not (yield from follow(bot, path + [w['start']], near=4.0)):
        return False
    for _ in range(120):
        s = bot.s
        if bailed(s):
            return False
        if abs((w['wall'][0] - s.pos[0]) * w['normal'][0] + (w['wall'][2] - s.pos[2]) * w['normal'][1]) < 7.0:
            break
        yield steer(s, w['wall'])
    yield from wait(bot, 3, pad('UP', 'CROSS'))
    for _ in range(40):
        s = bot.s
        if not s.air and s.st != WALL_RIDE and _ > 6:
            return True
        if s.st == WALL_RIDE:
            break
        off = abs((w['wall'][0] - s.pos[0]) * w['normal'][0] + (w['wall'][2] - s.pos[2]) * w['normal'][1])
        yield pad('CROSS', 'DOWN') if off < 2.6 else steer(s, w['wall'])
    if bot.s.st != WALL_RIDE:
        return True
    bot.tally('wall ride')
    for n in range(hold):
        if bot.s.st != WALL_RIDE:
            break
        # CROSS is the jump off the wall (wallRideJump): held, the ride ends at once; let go, it lasts while the wall
        # does, and its trick's animation can end on it (endWallTrickAnimation)
        yield core.NOTHING if WALL_LET_GO else pad('CROSS')
    if hold > 50 or WALL_LET_GO:
        bot.tally('wall ride held %d' % n)
    yield from wait(bot, 3)
    if bot.s.air:
        yield from air_trick(bot, plan)
    return True


def pole_entries(plan, part):
    """Upright edges (rails standing on end): poles to swing round."""
    lv, out = plan.lv, []
    for r in lv.rails:
        a, b = r['points'][0], r['points'][-1]
        if abs(a[1] - b[1]) < 3.0 or math.hypot(a[0] - b[0], a[2] - b[2]) > 1.0:
            continue
        node = lv.nearest((a[0], max(a[1], b[1]), a[2]), 2)
        if node is not None and lv.part.get(node) == part:
            out.append(dict(at=lv.world(node), name=r['name']))
    return out


def pole_line(bot, plan):
    """At a pole at speed, Circle held (routes.py vpole)."""
    part = plan.lv.main_part(bot.s.pos)
    if getattr(plan, 'pole_list', None) is None or plan.pole_list[0] != part:
        plan.pole_list = (part, pole_entries(plan, part))
    near = [p for p in plan.pole_list[1] if 15.0 < flat_dist(bot.s.pos, p['at']) < 120.0]
    if not near:
        return False
    p = bot.rng.choice(near)
    bot.tally('goal pole %s' % p['name'])
    path = plan.lv.path(bot.s.pos, p['at'])
    if path is None:
        return False
    if not (yield from follow(bot, path[:-5] or path, near=5.0)):
        return False
    for _ in range(90):
        s = bot.s
        if s.st in (VPOLE, VPOLE_A, HPOLE, VAULT):
            bot.tally('pole')
            while bot.s.st in (VPOLE, VPOLE_A, HPOLE, VAULT):
                yield pad('CIRCLE')
            yield from wait(bot, 3)
            if bot.s.air:
                yield from air_trick(bot, plan)
            return True
        if bailed(s):
            return False
        yield steer(s, p['at'], 'CIRCLE')
    return True


def vault_line(bot, plan):
    """Square at a waist-high edge, Circle held (routes.py vault)."""
    part = plan.lv.main_part(bot.s.pos)
    ledges = [e for e in plan.rail_entries(part) if 0.8 <= plan.lv.world(plan.lv.nearest(e['start'], 1))[1] - e['on'][1] <= 2.2]
    near = [e for e in ledges if 12.0 < flat_dist(bot.s.pos, e['on']) < 90.0]
    if not near:
        return False
    e = min(bot.rng.sample(near, min(5, len(near))), key=lambda q: flat_dist(bot.s.pos, q['on']))
    d = e['dir']
    side = 1.0 if bot.rng.random() < 0.5 else -1.0
    start = (e['on'][0] - d[2] * side * 10, e['start'][1], e['on'][2] + d[0] * side * 10)
    node = plan.lv.nearest(start, 1)
    if node is None or plan.lv.part.get(node) != part:
        return False
    bot.tally('goal vault %s' % e['name'])
    path = plan.lv.path(bot.s.pos, plan.lv.world(node))
    if path is None or not (yield from follow(bot, path, near=4.0)):
        return False
    for _ in range(100):
        s = bot.s
        if s.st == VAULT:
            bot.tally('vault')
            while bot.s.st == VAULT:
                yield pad('CIRCLE')
            yield from wait(bot, 3)
            if bot.s.air:
                yield from air_trick(bot, plan)
            return True
        if bailed(s) or flat_dist(s.pos, e['on']) < 1.0:
            return s.st == GROUND
        yield steer(s, e['on'], 'CIRCLE')
    return True


def cess_slide(bot):
    """A cess slide at speed (routes.py cess): R2 held."""
    if bot.s.st == GROUND and bot.s.speed > 10.0:
        bot.tally('cess')
        yield from wait(bot, 35, pad('UP', 'R2'))
    return True


def scene_objects(run):
    """The scene's object list (zge/scene/message.h): every object's address."""
    head = SYM['g_objectList']
    node, out = run.u32(head), []
    for _ in range(6000):
        if not node or node == head:
            break
        out.append(run.u32(node + 8))
        node = run.u32(node)
    return out


def object_pos(run, obj):
    return struct.unpack('<3f', run.mem(obj + 0x10, 12))         # SceneObject::position


def find_cars(bot):
    """What drives: objects that moved between two looks, in step, at a car's speed. (position, direction, speed) of each
    comes from `cars_now`."""
    run = bot.run
    objs = [o for o in scene_objects(run) if 0x100000 <= o < 0x2000000]
    a = {o: object_pos(run, o) for o in objs}
    yield from wait(bot, 20, pad('UP'))
    b = {o: object_pos(run, o) for o in objs}
    return [o for o in objs if 3.0 < math.dist(a[o], b[o]) < 25.0]


def skitch(bot, plan, limit=1500):
    """A ride behind a car (routes.py chase): alongside its lane ahead of it, the car comes past, CIRCLE held at its back."""
    cars = yield from find_cars(bot)
    if not cars:
        return False
    bot.tally('skitch: %d cars' % len(cars))
    last, target = {}, None
    for n in range(limit):
        s = bot.s
        if bailed(s):
            return False
        if s.st == SKITCH:
            bot.tally('skitch!')
            for _ in range(300):       # as long as the car will take it
                if bot.s.st != SKITCH:
                    break
                yield pad('CIRCLE')
            yield from wait(bot, 6)
            return True
        now = {o: object_pos(bot.run, o) for o in cars}
        best, cost = None, 1e18
        for o, p in now.items():
            q = last.get(o)
            if q is None or abs(p[1] - s.pos[1]) > 3.0:
                continue
            d = (p[0] - q[0], p[2] - q[2])
            size = math.hypot(*d)
            if size < 0.05 or size > 2.0:
                continue
            d = (d[0] / size, d[1] / size)
            rel = (s.pos[0] - p[0], s.pos[2] - p[2])
            along, side = rel[0] * d[0] + rel[1] * d[1], rel[1] * d[0] - rel[0] * d[1]
            c = abs(side) * 3.0 + (abs(along - 12.0) if along > -6.0 else 200.0 - along)
            if c < cost:
                best, cost = (p, d, along, side), c
        last = now
        if best is None:
            yield pad('UP')
            continue
        p, d, along, side = best
        sign = 1.0 if side >= 0 else -1.0
        off = 2.2 * sign
        if along > 5.0:                # ahead of it: beside its lane, its way, as fast as there is
            aim = (p[0] + d[0] * (along + 14.0) - d[1] * off, 0.0, p[2] + d[1] * (along + 14.0) + d[0] * off)
            yield steer(s, aim)
        else:                          # it comes past: in behind it
            aim = (p[0] - d[0] * 1.5, 0.0, p[2] - d[1] * 1.5)
            yield steer(s, aim, 'CIRCLE')
    return False


def launch_line(bot, plan):
    """At a ramp at speed, off the top, tricks in the air, back down."""
    r = next_ramp(plan, bot.s, bot.rng)
    if r is None:
        return False
    bot.tally('goal launch %s at %.0f %.0f' % (r.get('name', 'ramp'), r['top'][0], r['top'][2]))
    plan.tried[('ramp', r['foot'])] = plan.tried.get(('ramp', r['foot']), 0) + 1
    path = plan.lv.path(bot.s.pos, r['start'])
    if path is None:
        return False
    if not (yield from follow(bot, path + [r['start']], near=4.0, plan=plan)):
        bot.tally('launch: not there')
        return False
    for _ in range(240):
        s = bot.s
        if bailed(s):
            return False
        if s.air:
            bot.tally('launch')
            yield from air_trick(bot, plan)
            return True
        if flat_dist(s.pos, r['top']) < 1.5 and s.speed > 6:
            if 'name' in r and bot.rng.random() < 0.35:          # a hand plant on the lip (routes.py stall)
                bot.tally('plant')
                if PLAY_TRICKS:
                    yield from plant_held(bot)
                    continue
                yield from wait(bot, 60, pad('TRIANGLE'))
                continue
            yield from wait(bot, 3, pad('UP', 'CROSS'))
            continue
        yield steer(s, r['top'])
    return False


AIR_TOO_LONG = 300       # frames: no air lasts this long but a skater caught going up and down in a quarter pipe


def out_of_the_air(bot):
    """Caught in the air going up and down a quarter pipe (Industrial's at (-124, 14 up, 188) kept a skater in Air
    QPipe for minutes): the stick across the pipe until the skater is out, as a person would steer out of it."""
    bot.tally('caught in the air')
    side = 1.0 if bot.rng.random() < 0.5 else -1.0
    for n in range(600):
        if not bot.s.air or bot.s.down:
            return
        if n % 150 == 149:
            side = -side
        yield pad('UP', lx=side) if n % 40 < 30 else pad('UP', 'CROSS', lx=side)


def on_the_ground(bot, limit=600):
    """Anything still going on ends first: the air, a bail, a box."""
    n, aloft = 0, 0
    while n < limit and (bot.s.mode != PLAY or bot.s.down or bot.s.air or bot.s.grinding):
        n += 1
        aloft = aloft + 1 if bot.s.air else 0
        if aloft > AIR_TOO_LONG:
            yield from out_of_the_air(bot)
            aloft = 0
            continue
        if bot.s.mode == PAUSED:
            yield from close_boxes(bot)
        yield core.NOTHING


def roam(bot, plan):
    """No target in reach: somewhere else in this part of the level, else straight on and off whatever this is."""
    lv, part = plan.lv, plan.lv.main_part(bot.s.pos)
    cells = [k for k, p in lv.part.items() if p == part] if part is not None else []
    bot.tally('roam')
    if len(cells) > 60:
        for _ in range(6):
            goal = lv.world(bot.rng.choice(cells))
            if flat_dist(goal, bot.s.pos) > 25.0:
                path = lv.path(bot.s.pos, goal)
                if path:
                    return (yield from follow(bot, path, near=5.0, plan=plan))
    side = bot.rng.choice((-0.7, 0.0, 0.7))
    yield from wait(bot, 60, pad('UP', lx=side))
    yield from wait(bot, 3, pad('UP', 'CROSS'))
    yield from wait(bot, 40, pad('UP', lx=side))
    return True


# What an objective's words call a level's rails, where they differ (ai.py objectives, nav.py info).
RAIL_WORDS = (('traffic light', 'stoplight'), ('cliff edge', 'cliffedge'), ('boulder chain', 'chaingrind'),
              ('cable above', 'cablegrind0[34]'), ('switch', 'switch'), ('robot arm', 'robot'), ('octopus', 'octopus'),
              ('light wire', 'wire'), ('statue wing', 'wing'), ('clock', 'clock'), ('harpoon', 'harpoon'),
              ('tethered plane', 'Tether'), ('plane', 'plane'), ('pterodactyl', 'ptero'), ('mammoth', 'mammoth'),
              ('bronto skeleton spine', 'Backbone'), ('bronto rib', 'RibGrind'), ('bronto', 'bronto'),
              # (the levels' rail names, nav.Level rails: Boardwalk's, Cannery's, Hangar's, Museum's)
              ('wing nut', 'FerrisWheelNut'), ('mast', 'PirateShip.?M?Mast'), ('swordfish', 'swordfish'),
              ('control panel', 'controll|vatcontrolbox'), ('steam pipe', 'steampipe'), ('pier rope', 'RopeRail'),
              ('tower base', 'towerbase'), ('triceratops', 'triceratops'), ('display case', 'displaycase|longdisplay'),
              ('sign', 'sign'))


VARIETY = (rail_line, wall_line, launch_line, pole_line, vault_line)


CHAIN_WORDS = {'Bike Racks': 'barricade'}


# Objectives done on one object of the level's script (its .LOG names, the level's TGIH boxes): how a skater does it.
# Wall Ride the Broken Statue (Civic): the statue's own box (ScriptObj_VenusBefore, x 4.9..9.2, z -126.6..-124.3, from
# 9.3 up) stands at the east side of the walkway (7-8 up) that runs north from its giver; its WallRideStart event
# starts the audio sync whose syncStart completes the objective. Ridden north along the walkway, an ollie to the right
# into its west face, DOWN and CROSS to take the wall.
OBJECT_WALLS = {
    'Wall Ride the Broken Statue': dict(lanes=(2.0, 3.2), start_z=-140.0, jump_z=(-131.0, -130.2), face_x=4.9,
                                        face_z=(-126.6, -124.3), lx=0.33, reach=2.6, floor=-7.6),
}


def object_wall(bot, plan, spec, lane, jump_z):
    """One try at an object's wall face (OBJECT_WALLS): to the start, north along the lane, an ollie into the face."""
    start = (lane, spec['floor'], spec['start_z'])
    path = plan.lv.path(bot.s.pos, start)
    if path is None:
        return False
    if not (yield from follow(bot, path + [start], near=2.0)):
        yield from unstick(bot)
        return False
    ahead = lambda s: (lane, s.pos[1], s.pos[2] + 20)
    for _ in range(90):
        if abs(turn_to(bot.s, ahead(bot.s))) < 0.15:
            break
        yield steer(bot.s, ahead(bot.s))
    jumped = None
    for t in range(240):
        s = bot.s
        if s.st == WALL_RIDE:
            bot.tally('object wall ride')
            for n in range(240 if WALL_LET_GO else 60):
                if bot.s.st != WALL_RIDE:
                    break
                yield core.NOTHING if WALL_LET_GO else pad('CROSS')
            if WALL_LET_GO:
                bot.tally('object wall ride held %d' % n)
            break
        if jumped is None:
            if s.pos[2] > jump_z:
                jumped = t
                yield from wait(bot, 3, pad('UP', 'CROSS', lx=spec['lx']))     # an ollie: CROSS held, then let go
                continue
            yield steer(s, (lane, s.pos[1], s.pos[2] + 12))
        else:
            if s.pos[2] > spec['face_z'][1] + 3 or (not s.air and t > jumped + 8):
                break
            near = abs(spec['face_x'] - s.pos[0]) < spec['reach']
            yield pad('CROSS', 'DOWN', lx=spec['lx']) if near else pad('UP', lx=spec['lx'])
    yield from on_the_ground(bot)
    return True


def object_wall_goal(bot, plan, o, tries=9):
    """An OBJECT_WALLS objective: its giver first, then tries until the objective is over."""
    spec = OBJECT_WALLS[o['title']]
    if o['state'] == 'Init':
        pos = giver_of(plan, o)
        if pos is None or not (yield from talk(bot, plan, o, pos)):
            return False
        yield from wait(bot, 60)
        yield from close_boxes(bot)
    for n in range(tries):
        lanes, jumps = spec['lanes'], spec['jump_z']
        yield from object_wall(bot, plan, spec, lanes[n % len(lanes)], jumps[n // len(lanes) % len(jumps)])
        yield from wait(bot, 90)            # the audio sync's start (about a second) completes it
        now = objectives(bot.run)[o['index']]
        if now['done'] or now['state'] == 'Complete':
            bot.tally('object wall done: ' + o['title'])
            return True
    return False


# Objectives done by hand plants (Stall) on named edges high above a quarter pipe: how high a skater goes off a pipe is
# the jump, not the speed. Released on the face just under the lip, the jump button adds the jump speed (Physics /
# Jump, 16) straight up (groundControls), and leaving the face that frame (settleFrames 0) adds the face's own launch
# push (launchOffFace): Civic's train station facade launches at about 32 that way (13 above the lip, 3.6 up), where
# rolling off its lip gives 12 (2 above). The stall wants the grind button pressed while still rising, within Grind
# Radius QPipe of the edge, and Stall Timeout (0.5 s) since the last stall.
# Handplant the Gargoyle Heads (Civic): GargoyleAssigner's objective; each head's ScriptProp_gargoyle (edge 18.5 up,
# z 53.3) taps on its stallEnd (initializeAudioSync, then a camera shot), two taps complete it. The run-up is north
# along the head's x from a start on open ground (the +x head's line from z -12: south of it lies a sunken square).
HAND_PLANTS = {
    'Handplant the Gargoyle Heads': dict(heads=((-22.95, -56.0), (22.8, -12.0)), floor=0.0, jump_y=-3.6, hold_y=-2.0,
                                         plant_y=-14.0, low_lip=-2.5),
}
JUMP_LEAD = 0.025        # seconds: the jump button let go this far ahead of the height it is meant for


def lip_jump_launch(bot, spec, x, plants=True):
    """North along x up the quarter pipe ahead: CROSS held on the face, let go just under the lip (the jump), then in
    Air QPipe TRIANGLE pressed every other frame while rising past plant_y. Returns (launch speed, stalled)."""
    launch, stalled = None, False
    for t in range(900):
        s = bot.s
        ahead = (x, s.pos[1], s.pos[2] + 30)
        if s.st == STALL:
            stalled = True
            bot.tally('hand plant on an edge')
            for n in range(300):                 # balanced (balance[1], as a manual's) until it ends by itself
                if bot.s.st != STALL:
                    break
                yield pad(ly=stick(manual_stick(bot.s)))
            bot.tally('hand plant held %d' % n)
            break
        if s.st == AIR_QPIPE and launch is None and s.pos[1] > spec['low_lip']:
            yield steer(s, ahead)               # a low lip on the way, not the pipe meant
            continue
        if s.st == AIR_QPIPE:
            launch = launch or s.speed
            yield pad('TRIANGLE') if plants and s.vel[1] < 0 and s.pos[1] < spec['plant_y'] and t % 2 == 0 else core.NOTHING
            continue
        if launch is not None and not s.air:
            break
        if bailed(s) or s.mode != PLAY:
            break
        hold = not s.air and s.pos[1] < spec['hold_y'] and s.pos[1] + s.vel[1] * JUMP_LEAD > spec['jump_y']
        yield steer(s, ahead, *(('CROSS',) if hold else ()))
    yield from on_the_ground(bot)
    yield from recover(bot)
    return launch, stalled


def hand_plant_goal(bot, plan, o, tries=10):
    """A HAND_PLANTS objective: its giver first, then each edge in turn until the objective is over."""
    spec = HAND_PLANTS[o['title']]
    if o['state'] == 'Init':
        pos = giver_of(plan, o)
        if pos is None or not (yield from talk(bot, plan, o, pos)):
            return False
        yield from wait(bot, 60)
        yield from close_boxes(bot)
    left = list(spec['heads'])
    for n in range(tries):
        x, start_z = left[n % len(left)]
        start, behind = (x, spec['floor'], start_z), (x, spec['floor'], start_z - 10.0)    # met heading north
        path = plan.lv.path(bot.s.pos, behind)
        if path is None or not (yield from follow(bot, path + [behind, start], near=3.0)):
            yield from unstick(bot)
            continue
        launch, stalled = yield from lip_jump_launch(bot, spec, x)
        bot.tally('lip jump launch %.0f' % (launch or 0))
        if stalled:
            yield from wait(bot, 360)           # the tap's audio sync and camera shot
            if len(left) > 1:
                left.remove((x, start_z))
        now = objectives(bot.run)[o['index']]
        if now['done'] or now['state'] == 'Complete':
            bot.tally('hand plants done: ' + o['title'])
            return True
    return False


GIVER_NODES = {}


def giver_shown(bot, o, by_giver=False):
    """Whether an objective's giver is in the scene and drawn (zge/scene/node.h: nameHash +0x128, the name +0x12C,
    kNodeShown in the flags): a level's script keeps some givers hidden until another objective is done (Civic's
    busAssigner until the Big Ball's)."""
    run, h = bot.run, o['object']
    if by_giver and o['title'] in GIVER_NAMES:          # the assigner's own node, not the record's object
        h = GIVER_NAMES[o['title']]
        h = name_hash(h) if isinstance(h, str) else h
    key = (bot.s.level, h)
    if key not in GIVER_NODES:
        lo, hi = 0x200000, 0x2000000
        data = b''.join(run.mem(a, 0x100000) for a in range(lo, hi, 0x100000))
        needle, found, i = struct.pack('<I', h), [], -1
        while True:
            i = data.find(needle, i + 1)
            if i < 0:
                break
            node = lo + i - 0x128
            if node % 16 == 0 and i + 0x24 <= len(data):
                name = data[i + 4:i + 0x24].split(b'\0')[0]
                if name and name_hash(name.decode('latin1')) == h:
                    found.append(node)
        GIVER_NODES[key] = found
    nodes = GIVER_NODES[key]
    return not nodes or any(run.u32(n) & 1 for n in nodes)


def name_hash(name):
    """The engine's name hash (ai_level.py name_hash): case-insensitive ELF hash."""
    h = 0
    for ch in name.encode('latin1'):
        c = ch - 32 if 97 <= ch <= 122 else ch
        h = ((h << 4) + c) & 0xFFFFFFFF
        high = h & 0xF0000000
        if high:
            h ^= high >> 24
        h &= ~high & 0xFFFFFFFF
    return h


def rails_of(title):
    t = title.lower()
    if 'grind' not in t:
        return None
    for words, pattern in RAIL_WORDS:
        if words in t:
            return pattern
    return None


def levels_points(s):
    return s.challenge_points[s.level] if 0 <= s.level < 14 else 0


# Objectives whose giver is another script object than the one the Objective record names (the level scripts'
# assigners: the record names the level's own object), by title: the giver's script object name or name hash.
GIVER_NAMES = {'Clear the Junkyard': 0x98BAF2, 'Handplant the Gargoyle Heads': 'GargoyleAssigner',
               'Grind the 3 Wires to the Big Ball': 'globeAssigner', 'Grind the Fireworks Rail': 'fireworksAssigner'}


def giver_of(plan, o, attempt=0):
    """Where to find an objective's giver: names are shared, so a later attempt tries the next point of that name."""
    name = GIVER_NAMES.get(o['title'], o['object'])
    h = name_hash(name) if isinstance(name, str) else name
    found = [p['pos'] for p in plan.lv.points if p['name'] == h and p['cls'] != 0]
    return found[attempt % len(found)] if found else None


def path_beside(plan, start, pos, radius=2.5):
    """A way to a floor cell within radius of pos when pos's own cell has none from here: a giver stands against a
    wall, and the cell under it is the far side's (Industrial's timed run giver at the punch clock)."""
    lv = plan.lv
    ci, ck = nav.cell_of(pos[0], pos[2])
    near = []
    for di in range(-2, 3):
        for dk in range(-2, 3):
            for n, (y, _) in enumerate(lv.nodes.get((ci + di, ck + dk), ())):
                w = lv.world(((ci + di, ck + dk), n))
                if flat_dist(w, pos) <= radius and abs(y - pos[1]) < 2.0:
                    near.append(w)
    for w in sorted(near, key=lambda w: flat_dist(w, pos))[:6]:
        path = lv.path(start, w)
        if path:
            return path + [w]
    return None


def stop_at(bot, pos, near=2.5, limit=900):
    """Brake to a stand next to pos (a giver talks to a skater who stops by, not one rolling past at speed)."""
    for _ in range(limit):
        s = bot.s
        if bailed(s):
            return False
        d = flat_dist(s.pos, pos)
        if d < near and s.speed < 3.0:
            return True
        if d < near + s.speed * 0.6 or abs(turn_to(s, pos)) > 1.2:
            yield pad('DOWN', lx=stick(-turn_to(s, pos) * 2.4))       # DOWN on the ground slows the skater
        else:
            yield steer(s, pos)
    return False


def talk(bot, plan, o, pos):
    """To a giver and CIRCLE until the objective moves on."""
    bot.tally('talk: ' + o['title'])
    path, stand = plan.lv.path(bot.s.pos, pos), pos
    if path is None:
        path = path_beside(plan, bot.s.pos, pos)
        stand = path[-1] if path else pos
    if path is None:
        return False
    if not (yield from follow(bot, path[:-1] or path, near=6.0)):
        plan.close_near(bot.s.pos)          # a crash on the way: no gate here after all
        return False
    if not (yield from stop_at(bot, stand)):
        return False
    for _ in range(6):
        yield from tap(bot, 'CIRCLE', hold=3, gap=40)
        yield from close_boxes(bot)
        now = objectives(bot.run)[o['index']]
        if now['state'] != o['state']:
            bot.tally('talked: ' + o['title'] + ' -> ' + now['state'])
            return True
    return False


def points_in(title):
    """The points a title asks for: '20,000 Point Trick' -> 20000."""
    m = re.search(r'([\d,]{5,}) ?Point', title)
    return int(m.group(1).replace(',', '')) if m else 0


def note_done(bot, obs, done):
    for o in obs:
        if o['done'] and o['index'] not in done:
            done.add(o['index'])
            bot.tally('DONE %s (%d) tick %d score %d' % (o['title'], o['points'], bot.s.tick, bot.s.score))
            print('objective done:', o['title'], 'challenge points', levels_points(bot.s), 'tick', bot.s.tick, 'score', bot.s.score, flush=True)


def timed_run(bot, plan, o, limit=7200, attempt=0):
    """A timed run: the giver starts it; lines that bank until the objective says it is over."""
    pos = giver_of(plan, o, attempt)
    if pos is None or not (yield from talk(bot, plan, o, pos)):
        return False
    want = points_in(o['title'])
    end = bot.s.tick + limit
    bot.tally('run: ' + o['title'])
    while bot.s.tick < end:
        yield from on_the_ground(bot)
        now = objectives(bot.run)[o['index']]
        if now['done'] or now['state'] in ('Init', 'Complete'):
            break
        if not (yield from long_line(bot, plan, most=30, bank=want * 1.15)):
            yield from unstick(bot)
    now = objectives(bot.run)[o['index']]
    bot.tally('run over: %s done %s state %s score %d' % (o['title'], now['done'], now['state'], bot.s.score))
    return True


WANT = int(os.environ.get('PLAY_WANT', '8'))       # a level's challenge points before the career moves on


def career_level(bot, plan, ticks, want=None, rounds=None):
    """The level's objectives until `want` challenge points: score and lines all the while, a run's giver or a named
    rail in between."""
    yield from until(bot, lambda s: s.speed > 0.2, 900, pad('UP'))    # the level's opening shot takes no input
    want = want or int(os.environ.get('PLAY_WANT_' + plan.lv.name, WANT))     # PLAY_WANT_CIVIC: one level's own
    end = bot.s.tick + ticks
    done, spoken, missed = set(), {}, {}
    n = 0
    while bot.s.tick < end and levels_points(bot.s) < want:
        yield from on_the_ground(bot)
        n += 1
        obs = objectives(bot.run)
        note_done(bot, obs, done)
        open_ = [o for o in obs if not o['done'] and not o['locked']]
        ok = None
        for o in open_ if n % 2 == 0 else ():
            if o['title'] in OBJECT_WALLS and missed.get(o['index'], 0) < 3 and giver_shown(bot, o):
                missed[o['index']] = missed.get(o['index'], 0) + 1
                ok = yield from object_wall_goal(bot, plan, o)
                break
            if o['title'] in HAND_PLANTS and missed.get(o['index'], 0) < 3 and giver_shown(bot, o, by_giver=True):
                missed[o['index']] = missed.get(o['index'], 0) + 1
                ok = yield from hand_plant_goal(bot, plan, o)
                break
        for o in open_ if ok is None else ():
            if points_in(o['title']) and 'Construct' not in o['title'] and giver_of(plan, o) and o['state'] == 'Init' and spoken.get(o['index'], 0) < 12 and n >= spoken.get(('next', o['index']), 0) and n >= spoken.get('next', 0) and giver_shown(bot, o):
                spoken[o['index']] = spoken.get(o['index'], 0) + 1
                spoken[('next', o['index'])] = n + 3
                ok = yield from timed_run(bot, plan, o, attempt=spoken[o['index']] - 1)
                if not ok:
                    spoken['next'] = n + 3          # a giver out of reach: lines in between before the next try
                break
        for o in open_ if ok is None and n % 2 == 1 else ():
            pattern = rails_of(o['title'])
            if pattern and missed.get(pattern, 0) < 4 and o['state'] == 'Active':
                missed[pattern] = missed.get(pattern, 0) + 1
                ok = yield from rail_line(bot, plan, pattern)
                break
        for o in open_ if ok is None and n % 3 == 2 else ():
            m = re.match(r'(\d+) (.*) in (1|a Single) Trick', o['title'])
            if m and o['state'] == 'Active' and missed.get(o['index'], 0) < 12:
                missed[o['index']] = missed.get(o['index'], 0) + 1
                ok = (yield from rail_chain(bot, plan, CHAIN_WORDS.get(m.group(2)), int(m.group(1)))) > 0
                break
        for o in open_ if ok is None and n % 3 == 1 else ():
            if o['title'].startswith('Skitch') and o['state'] == 'Active' and missed.get(o['index'], 0) < 14:
                missed[o['index']] = missed.get(o['index'], 0) + 1
                ok = yield from skitch(bot, plan, 900)
                break
        if ok is None and n % 3 == 0 and plan.doubles():
            ok = yield from double_line(bot, plan)
        if ok is None and n % 4 == 0 and os.environ.get('PLAY_VARIETY'):      # the other things a skater does, between the lines
            ok = yield from VARIETY[(n // 4) % len(VARIETY)](bot, plan)
            if n % 12 == 0:
                yield from cess_slide(bot)
        if ok is None:
            ok = yield from long_line(bot, plan)
        yield core.NOTHING
        if not ok:
            bot.tally('fail')
            yield from on_the_ground(bot)
            if bot.s.speed < 3.0:
                yield from unstick(bot)
    note_done(bot, objectives(bot.run), done)
    print('level', bot.s.level, 'challenge points', levels_points(bot.s), 'goals', bot.s.goals[bot.s.level] if bot.s.level < 14 else 0,
          'open %#x' % (bot.s.open_levels & 0xFFFFFFFF), 'score', bot.s.score, 'tick', bot.s.tick, flush=True)


# The pad script's clock only runs with a script (ps2_pad.cpp applyScript), and core.play and Recorder go by
# it: a live run has this one item, past any end, for its clock.
CLOCK_ONLY = '99999999v:SELECT:1v'
# Sessions are played at the A/B gate's clock (career_ab.py GATE_CLOCK), so one replays the same in the gate.
GATE_CLOCK = int(os.environ.get('PLAY_CLOCK', '4096'))


def config(name, env=None, secs=900, gs=False, script=CLOCK_ONLY):
    e = {'PS2X_SAVES': 'card', 'PS2X_EE_SCALE': str(GATE_CLOCK)}
    # A run that records (PS2X_FNREC) has readable code off, as dm2.py's and bmx.py's runs always do: readable
    # functions that are on call each other past the recorder (the recorder refuses otherwise). A run that does not
    # record plays the defaults, the readable code a play test is for.
    if os.environ.get('PS2X_FNREC'):
        e['PS2X_READABLE'] = 'off'
    if os.environ.get('PLAY_FNPROF'):     # the functions a run calls (runtime ee/profile.cpp): calls per function, CSV
        os.makedirs(os.path.join(SCRATCH, 'prof'), exist_ok=True)
        e['PS2X_FNPROF'] = os.path.join(SCRATCH, 'prof', name + '.csv')
    e.update(env or {})
    return core.RunConfig(name=name, runner=RUNNER, elf=ELF, root=os.path.join(SCRATCH, 'runs'), env=e,
                          max_secs=secs, gs=gs, script=script)


class Log:
    """Each read as a line of samples, for `report`."""

    def __init__(self, path):
        self.f = gzip.open(path, 'wt', compresslevel=3)
        self.n = 0

    def __call__(self, s, p):
        r = dict(t=s.tick, s=s.tick, m=s.mode, pn=s.panel, l=s.level, f=s.frame)
        if s.player:
            r.update(p=s.player, st=s.st, sub=s.substate, land=s.landing, fg=s.flags, x=s.pos, v=s.vel, sp=s.speed,
                     air=s.air_time, crash=s.crash, pend=s.pending, mul=s.multiplier, sk=s.skater, score=s.score, j=round(s.juice, 3), js=s.carton)
            if s.st == MANUAL or s.grinding:
                r.update(b=round(s.balance, 3), bm=round(s.manual_balance, 3), ds=round(s.drift, 4), ms=round(s.manual_speed, 4), lx=p.lx, ly=p.ly)
            if self.n % 30 == 0:
                r.update(open=s.open_levels, goals=s.goals, rew=s.rewards, cc=s.completion)
        self.n += 1
        self.f.write(json.dumps(r, separators=(',', ':')) + '\n')

    def close(self):
        self.f.close()


def session(name, k, level, ticks, seed=1, skater=0, gs=False, env=None, real=False):
    env = dict(env or {})
    if not real:
        env['PS2X_DEV_UNLOCK_ALL'] = '1'
    plan = Plan(level)

    def behaviour(bot):
        yield from boot(bot)
        yield from career(bot, k, skater)
        print('placed tick', bot.s.tick, 'level', bot.s.level, 'skater', bot.s.skater, 'at', [round(v, 1) for v in bot.s.pos], flush=True)
        yield from career_level(bot, plan, ticks)

    rec = core.Recorder()
    log = Log(os.path.join(SCRATCH, 'measure', name + '.samples.gz'))
    trace = core.Trace(os.path.join(SCRATCH, 'sessions', name + '.trace'), every=60)
    with core.Run(config(name, env, secs=max(300, ticks // 40), gs=gs)) as run:
        bot = Player(run, behaviour, seed)
        try:
            core.play(run, read, bot, ticks + 12000, on_tick=log, recorder=rec, trace=trace)
        finally:
            log.close()
            trace.close()
        print('tick', run.tick, 'at', [round(v, 1) for v in bot.s.pos], 'notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:12], flush=True)
    with open(os.path.join(SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(rec.text())
    with open(os.path.join(SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)


ENTRY_OF_LEVEL = {5: 1, 0: 2, 4: 3, 1: 4, 2: 5, 3: 6, 6: 7}    # the level select's entry of a level number


def career_session(name, ticks, seed=1, skater=0, first=1, most=8, gs=False):
    """A skater through the career on a fresh card: a level's objectives to 8 points, the next level opened, on to it."""
    rec = core.Recorder()
    log = Log(os.path.join(SCRATCH, 'measure', name + '.samples.gz'))
    trace = core.Trace(os.path.join(SCRATCH, 'sessions', name + '.trace'), every=60)
    results = []
    unlock = os.environ.get('PLAY_UNLOCK') == '1'

    def behaviour(bot):
        yield from boot(bot)
        yield from career(bot, first, skater)
        k = first
        for _ in range(most):
            plan = Plan(nav.LEVELS[k])
            print('level entry', k, nav.LEVELS[k], 'number', bot.s.level, 'tick', bot.s.tick, 'open %#x' % (bot.s.open_levels & 0xFFFFFFFF), flush=True)
            before = bot.s.open_levels
            yield from career_level(bot, plan, ticks)
            yield from on_the_ground(bot)
            points = levels_points(bot.s)
            results.append((nav.LEVELS[k], points, bot.s.tick, bot.s.score))
            opened = (bot.s.open_levels & ~before) & 0xFFFFFFFF
            print('left level', nav.LEVELS[k], 'points', points, 'opened %#x' % opened, flush=True)
            news = [ENTRY_OF_LEVEL[b] for b in range(16) if opened >> b & 1 and b in ENTRY_OF_LEVEL]
            if unlock and k + 1 < len(nav.LEVELS):
                news = [k + 1]          # (PLAY_UNLOCK=1) every level open: the next entry in the level select's order
            elif points < 8 or not news:
                return
            yield from to_level_select(bot)
            k = news[0]
            yield from next_level(bot, k)

    # PLAY_UNLOCK=1 (test only): PS2X_DEV_UNLOCK_ALL, each level after the last whatever its points, so the levels
    # past the player's limit are played as career levels with their goals (FIRST: the entry to start at)
    env = {'PS2X_DEV_UNLOCK_ALL': '1'} if unlock else {}
    with core.Run(config(name, env, secs=max(300, (ticks + 2000) * most // 30))) as run:
        bot = Player(run, behaviour, seed)
        try:
            core.play(run, read, bot, (ticks + 6000) * most + 8000, on_tick=log, recorder=rec, trace=trace)
        finally:
            log.close()
            trace.close()
        print('tick', run.tick, 'results', results, 'notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:10], flush=True)
    with open(os.path.join(SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(rec.text())
    with open(os.path.join(SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)


def reload_session(name, saves, profile=None, k=1, ticks=600):
    """A career saved in native files (SAVES: its folder) played on: boot, CAREER, level select entry k, placed; the
    profile's open levels, goals and completion as the game holds them then (PROFILE: another one, a fresh career)."""
    env = {'PS2X_SAVES': None, 'PS2X_SAVES_DIR': saves}
    if profile:
        env['PS2X_SAVE_PROFILE'] = profile
    out = {}

    def behaviour(bot):
        yield from boot(bot)
        yield from career(bot, k)
        out.update(level=bot.s.level, open='%#x' % (bot.s.open_levels & 0xFFFFFFFF), goals=list(bot.s.goals),
                   completion=round(bot.s.completion, 2), points=levels_points(bot.s), placed=bot.s.placed)
        yield from wait(bot, ticks)

    with core.Run(config(name, env, secs=600)) as run:
        core.play(run, read, Player(run, behaviour), 20000)
    print(name, 'profile', profile or 'Career', json.dumps(out), flush=True)
    return out


def objectives_page(bot):
    """SELECT in a career level: the objectives page (checkPauseButtons), then back to the level."""
    mode = bot.s.mode
    yield from tap(bot, 'SELECT', hold=6, gap=90)
    bot.tally('select: mode %d -> %d' % (mode, bot.s.mode))
    for _ in range(3):
        if bot.s.mode == PLAY:
            break
        yield from tap(bot, 'SELECT', hold=6, gap=60)
    yield from close_boxes(bot)
    yield from until(bot, lambda s: s.mode == PLAY, 600)


def goals_visit(bot, plan, ticks, walls=4):
    """One visit of a career level for the play goals: the objectives page, wall rides held to their end, then the level's
    objectives and lines."""
    yield from until(bot, lambda s: s.speed > 0.2, 900, pad('UP'))
    yield from objectives_page(bot)
    for _ in range(walls):
        if not (yield from wall_line(bot, plan, hold=400)):
            yield from unstick(bot)
        yield from on_the_ground(bot)
    yield from career_level(bot, plan, ticks)


def goals_session(name, ticks, seed=1, skater=0, visits=3):
    """A fresh card's career for the play goals: Movie Lot, left for the level select and entered again `visits` times
    (each leaving saves the level: the second save frees the first's blocks), SELECT's objectives page, held wall rides."""
    rec = core.Recorder()
    log = Log(os.path.join(SCRATCH, 'measure', name + '.samples.gz'))
    trace = core.Trace(os.path.join(SCRATCH, 'sessions', name + '.trace'), every=60)
    k = 1

    def behaviour(bot):
        yield from boot(bot)
        yield from career(bot, k, skater)
        for v in range(visits):
            plan = Plan(nav.LEVELS[k])
            print('visit', v, nav.LEVELS[k], 'tick', bot.s.tick, flush=True)
            yield from goals_visit(bot, plan, ticks)
            yield from on_the_ground(bot)
            print('left', nav.LEVELS[k], 'points', levels_points(bot.s), 'tick', bot.s.tick, 'score', bot.s.score, flush=True)
            yield from to_level_select(bot)
            yield from next_level(bot, k)
        yield from wait(bot, 300)

    env = {}
    with core.Run(config(name, env, secs=max(300, (ticks + 6000) * (visits + 1) // 30))) as run:
        bot = Player(run, behaviour, seed)
        try:
            core.play(run, read, bot, (ticks + 8000) * (visits + 1) + 8000, on_tick=log, recorder=rec, trace=trace)
        finally:
            log.close()
            trace.close()
        print('tick', run.tick, 'notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:16], flush=True)
    with open(os.path.join(SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(rec.text())
    with open(os.path.join(SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)


# ---- split screen ------------------------------------------------------------------------------
# A zgui screen is known by the name hash of the top panel's parent group (tools/routes.py screen): the pause, split
# screen's game choice (MultiplayerStart) and its results (MultiplayerOverStats).
SCREEN_PAUSE, SCREEN_MP_MENU, SCREEN_MP_RESULTS = 0x546A75, 0x29AAAB4, 0x6BA1013
MP_GAMES = ('Most Points', 'Best Trick', 'Egg Hunt', 'Twenty One', 'Animal Rescue')
PROP_NEXT, PROP_POS, PROP_KIND, PROP_ENABLED = 0x7C, 0x40, 0x58, 0x64    # level/props.h Prop: the list at g_props
PROP_PARROT, PROP_SLOTH = 0x2E, 0x30                                      # the rescue animals: parrot, monkey, sloth
CARRIED = 0x894                                                           # Player::carriedAnimalCount (u16; skater.h)


def screen(bot):
    p = bot.s.panel
    return bot.run.u32(bot.run.u32(p + 8) + 0x10) if p else 0


def vmenu(bot, k, most=20):
    """A vertical zgui menu's cursor to entry k."""
    for _ in range(most):
        c = bot.s.cursor
        if c == k or c < 0 or c > 255:
            break
        yield from tap(bot, 'DOWN' if c < k else 'UP', hold=6, gap=20)
    return bot.s.cursor == k


def mp_to_menu(bot):
    """In split screen: to the Multiplayer menu (the game choice): START, GAME SELECT, past the results."""
    for _ in range(16):
        scr = screen(bot)
        if bot.s.mode == PAUSED and scr == SCREEN_MP_MENU:
            return True
        if bot.s.mode == PLAY:
            yield from tap(bot, 'START', gap=40)
        elif scr == SCREEN_PAUSE:
            yield from vmenu(bot, 1)
            yield from tap(bot, 'CROSS', gap=90)
        else:
            yield from tap(bot, 'CROSS', gap=60)
    return bot.s.mode == PAUSED and screen(bot) == SCREEN_MP_MENU


def animals(bot):
    """The rescue animals still out: [(prop, kind, position)]."""
    run, out = bot.run, []
    prop = run.u32(SYM['g_props'])
    for _ in range(600):
        if not prop:
            break
        kind = run.u32(prop + PROP_KIND)
        if PROP_PARROT <= kind <= PROP_SLOTH and run.u32(prop + PROP_ENABLED):
            out.append((prop, kind, struct.unpack('<3f', run.mem(prop + PROP_POS, 12))))
        prop = run.u32(prop + PROP_NEXT)
    return out


def carried(bot):
    return struct.unpack('<H', bot.run.mem(bot.s.player + CARRIED, 2))[0] if bot.s.player else 0


def rescue(bot, plan, limit):
    """Animal Rescue: to the nearest animal out (three at most carried), through its box; no tricks while carrying
    (a bail lets them go); round and round once three are held, until the clock ends the game."""
    end = bot.s.tick + limit
    skip = {}
    while bot.s.tick < end and bot.s.mode == PLAY:
        yield from on_the_ground(bot)
        if bot.s.mode != PLAY:
            break
        held = carried(bot)
        out = [a for a in animals(bot) if skip.get(a[0], 0) < 3]
        if held >= 3 or not out:
            path = far_path(plan, bot.s.pos, bot.rng, least=30.0)
            if not path or not (yield from follow(bot, path[:12], near=4.0, limit=600)):
                yield from unstick(bot)
            continue
        prop, kind, at = min(out, key=lambda a: flat_dist(bot.s.pos, a[2]))
        path = plan.lv.path(bot.s.pos, at)
        if path is None:
            skip[prop] = 3
            continue
        bot.tally('animal %d at %.0f %.0f' % (kind, at[0], at[2]))
        ok = yield from follow(bot, path + [at], near=1.5, limit=1200)
        for _ in range(40):         # through the box, slowly
            if carried(bot) > held or bot.s.mode != PLAY:
                break
            yield steer(bot.s, at)
        if carried(bot) > held:
            bot.tally('picked up (%d held)' % carried(bot))
            print('animal picked up: held', carried(bot), 'tick', bot.s.tick, flush=True)
        else:
            skip[prop] = skip.get(prop, 0) + 1
            if not ok:
                yield from unstick(bot)


SPLIT_MENU = ('400v:START:10v,440v:START:10v,480v:START:10v,900v:RIGHT:6v,940v:RIGHT:6v,980v:RIGHT:6v,'
              + ','.join('%dv:CROSS:6v' % (1040 + 90 * i) for i in range(4)) + ',1355v:RIGHT:6v,'
              + ','.join('%dv:CROSS:6v' % (1400 + 90 * i) for i in range(6)))


def mp_session(name, game, ticks, seed=1, rounds=1):
    """Split screen (port 1 mirrors port 0), the Multiplayer menu's game `game` played `rounds` times to its results."""
    rec = core.Recorder()
    log = Log(os.path.join(SCRATCH, 'measure', name + '.samples.gz'))
    trace = core.Trace(os.path.join(SCRATCH, 'sessions', name + '.trace'), every=60)
    env = {'PS2X_PAD_PORT1': 'mirror'}

    def behaviour(bot):
        # The menus are a pad script (SPLIT_MENU): player two's choices are port 1's, which the link does not reach
        # and the script does (PS2X_PAD_PORT1=mirror: the script's presses go to every port).
        last = None
        for _ in range(6000):
            if bot.s.mode == PLAY and bot.s.placed:
                break
            if VERBOSE and (bot.s.mode, screen(bot), bot.s.cursor) != last:
                last = (bot.s.mode, screen(bot), bot.s.cursor)
                print('menu: time', bot.run.script_time, 'mode', last[0], 'screen %#x' % last[1], 'cursor', last[2], flush=True)
            yield core.NOTHING
        yield from wait_placed(bot)
        print('split screen placed tick', bot.s.tick, 'level', bot.s.level, flush=True)
        plan = Plan(nav.LEVELS[ENTRY_OF_LEVEL.get(bot.s.level, 1)])
        for r in range(rounds):
            if not (yield from mp_to_menu(bot)):
                print('no Multiplayer menu: mode', bot.s.mode, 'screen %#x' % screen(bot), flush=True)
                return
            yield from vmenu(bot, game)
            print('game', game, MP_GAMES[game], 'cursor', bot.s.cursor, 'tick', bot.s.tick, flush=True)
            yield from tap(bot, 'CROSS', gap=90)
            yield from close_boxes(bot)
            yield from until(bot, lambda s: s.mode == PLAY, 3000)
            yield from wait(bot, 60)
            print('round', r, 'animals out', len(animals(bot)), 'tick', bot.s.tick, flush=True)
            if game == 4:
                yield from rescue(bot, plan, ticks)
            else:
                yield from career_level(bot, plan, ticks, want=99)
            print('round', r, 'over: mode', bot.s.mode, 'screen %#x' % screen(bot), 'held', carried(bot), 'points', bot.s.points,
                  'tick', bot.s.tick, flush=True)
            yield from until(bot, lambda s: s.mode != PLAY, 30000, pad('UP'))
            yield from wait(bot, 240)
            print('results screen %#x' % screen(bot), 'tick', bot.s.tick, flush=True)
        yield from mp_to_menu(bot)
        yield from wait(bot, 120)

    menu = os.environ.get('SPLIT_SCRIPT', SPLIT_MENU)
    with core.Run(config(name, env, secs=max(300, ticks * rounds // 20), gs=bool(os.environ.get('PLAY_SHOTS')),
                         script=menu + ',' + CLOCK_ONLY)) as run:
        bot = Player(run, behaviour, seed)
        try:
            core.play(run, read, bot, (ticks + 8000) * rounds + 6000, on_tick=log, recorder=rec, trace=trace)
        finally:
            log.close()
            trace.close()
        print('tick', run.tick, 'notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:16], flush=True)
    with open(os.path.join(SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(menu + ',' + rec.text())
    with open(os.path.join(SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)


# ---- the Park Editor ---------------------------------------------------------------------------
# (tools/routes.py pe_*: the tool roll of 16 entries, R1 next / L1 previous: 0 Terrain, 1-13 the Object Tool's piece
# groups, 14 Rail, 15 Start; Terrain: X select, d-pad UP/DOWN held raises/lowers, O drops; Object: X place, Square
# delete; the pause: Continue, Test Run, ...; a test run's pause: Continue, Sound Options, Return to Editor.)
PE_ROLL = 16
PE_EDITOR = SYM['g_parkEditorCore']                # the editor object while the editor runs (routes.py PE_EDITOR)


def pe_press(bot, button, hold=6, gap=40):
    yield from tap(bot, button, hold=hold, gap=gap)


def pe_taps(bot, moves, gap=20):
    names = {'R': 'RIGHT', 'L': 'LEFT', 'U': 'UP', 'D': 'DOWN'}
    for m in moves.split():
        for _ in range(int(m[1:] or 1)):
            yield from tap(bot, names[m[0]], hold=6, gap=gap)


def pe_roll(bot, k):
    at = getattr(bot, 'pe_at', 0)
    fwd = (k - at) % PE_ROLL
    button, n = ('R1', fwd) if fwd <= PE_ROLL // 2 else ('L1', PE_ROLL - fwd)
    for _ in range(n):
        yield from tap(bot, button, hold=6, gap=50)
    yield from wait(bot, 20)
    bot.pe_at = k


def pe_editing(bot, limit=3600):
    yield from until(bot, lambda s: s.mode == PLAY and bot.run.u32(PE_EDITOR) != 0, limit)
    yield from wait(bot, 300)
    return bot.s.mode == PLAY and bot.run.u32(PE_EDITOR) != 0


def pe_enter(bot, theme=0):
    """Main menu > PARK EDITOR (the menu does not wrap: RIGHT to OPTIONS, LEFT once), the first skater, New Park,
    theme `theme`; in the editor."""
    for _ in range(6):
        yield from tap(bot, 'RIGHT', hold=6, gap=30)
    yield from tap(bot, 'LEFT', hold=6, gap=30)
    p0 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=60)
    p1 = yield from new_panel(bot, p0)
    yield from tap(bot, 'CROSS', gap=60)
    p2 = yield from new_panel(bot, p1)
    if bot.s.cursor == NAME_ENTRY_END:
        yield from tap(bot, 'CROSS', gap=60)
        p2 = yield from new_panel(bot, p2)
    yield from vmenu(bot, 0)                       # New Park
    p3 = bot.s.panel
    yield from tap(bot, 'CROSS', gap=60)
    yield from new_panel(bot, p3)
    yield from set_cursor(bot, theme)
    yield from tap(bot, 'CROSS', gap=60)
    bot.pe_at = 0
    return (yield from pe_editing(bot))


def pe_towers(bot):
    """Towers (routes.py pe_towers): a block of terrain raised (or a pit lowered), a piece placed on it, the ground
    raised and lowered under the piece, the piece deleted and placed again; four of them along the row."""
    for g, up, down in ((1, 30, 0), (4, 50, 20), (8, 0, 30), (11, 70, 60)):
        yield from pe_taps(bot, 'U4')
        yield from pe_roll(bot, 0)
        yield from tap(bot, 'CROSS', gap=40)
        if up:
            yield from wait(bot, up, pad('UP'))
            yield from wait(bot, 10)
        if down and not up:
            yield from wait(bot, down, pad('DOWN'))
            yield from wait(bot, 10)
        yield from tap(bot, 'CIRCLE', gap=40)
        yield from pe_roll(bot, g)
        yield from tap(bot, 'CROSS', gap=60)
        yield from wait(bot, 10)
        yield from pe_roll(bot, 0)
        yield from tap(bot, 'CROSS', gap=40)
        yield from wait(bot, 25, pad('UP'))
        yield from wait(bot, 10)
        yield from wait(bot, down or 15, pad('DOWN'))
        yield from wait(bot, 10)
        yield from tap(bot, 'CIRCLE', gap=40)
        yield from pe_roll(bot, g)
        yield from tap(bot, 'SQUARE', gap=60)
        yield from wait(bot, 10)
        yield from tap(bot, 'CIRCLE', gap=30)
        yield from tap(bot, 'CROSS', gap=60)
        yield from wait(bot, 10)
        bot.tally('tower %d' % g)


def blind_skate(bot, ticks):
    """Skating a park the player has no plan of: rolling, turning away when stopped, ollies with a grab, manuals,
    a grind button held over anything in reach; up again after a bail."""
    end = bot.s.tick + ticks
    turn = 1.0
    while bot.s.tick < end and bot.s.mode in (PLAY, PAUSED):
        s = bot.s
        if s.mode == PAUSED:
            yield from close_boxes(bot)
            continue
        if bailed(s):
            yield from recover(bot)
            continue
        if s.speed < 1.5:
            turn = -turn if bot.rng.random() < 0.5 else turn
            yield from wait(bot, 20, pad('DOWN', lx=turn))
            yield from wait(bot, 30, pad('UP', lx=turn))
            continue
        r = bot.rng.random()
        if r < 0.03:
            yield from wait(bot, 3, pad('UP', 'CROSS'))
            yield from wait(bot, 6, pad('UP'))
            yield from wait(bot, 2, pad(bot.rng.choice(('LEFT', 'RIGHT', 'UP', 'DOWN'))))
            yield from wait(bot, 14, pad('SQUARE', 'TRIANGLE'))
            yield from on_the_ground(bot, 120)
            bot.tally('blind ollie')
        elif r < 0.04:
            yield from wait(bot, 2, pad('UP'))
            yield from wait(bot, 2)
            yield from wait(bot, 2, pad('DOWN'))
            yield from wait(bot, 40, pad(ly=manual_stick(s)))
            bot.tally('blind manual')
        else:
            yield pad('UP', lx=stick(math.sin(s.tick / 90.0) * 0.5))


def pe_session(name, ticks, seed=1, theme=0):
    """The Park Editor on a fresh card: a new park, towers built, a Test Run skated, back to the editor."""
    rec = core.Recorder()
    log = Log(os.path.join(SCRATCH, 'measure', name + '.samples.gz'))
    trace = core.Trace(os.path.join(SCRATCH, 'sessions', name + '.trace'), every=60)
    env = {}

    def behaviour(bot):
        yield from boot(bot)
        ok = yield from pe_enter(bot, theme)
        print('park editor:', 'editing' if ok else 'NOT editing', 'tick', bot.s.tick, flush=True)
        if not ok:
            return
        yield from pe_towers(bot)
        print('towers built tick', bot.s.tick, flush=True)
        yield from tap(bot, 'START', gap=60)            # the pause: Test Run
        yield from vmenu(bot, 1)
        yield from tap(bot, 'CROSS', gap=60)
        yield from until(bot, lambda s: s.placed, 3000)
        yield from wait(bot, 60)
        yield from close_boxes(bot)
        print('test run at', [round(v, 1) for v in bot.s.pos], 'tick', bot.s.tick, flush=True)
        yield from blind_skate(bot, ticks)
        print('test run over: score', bot.s.score, 'tick', bot.s.tick, flush=True)
        yield from tap(bot, 'START', gap=60)            # the test run's pause: Return to Editor
        yield from vmenu(bot, 2)
        yield from tap(bot, 'CROSS', gap=60)
        back = yield from pe_editing(bot)
        print('back in the editor' if back else 'NOT back in the editor', 'tick', bot.s.tick, flush=True)
        yield from wait(bot, 120)

    with core.Run(config(name, env, secs=max(300, ticks // 20))) as run:
        bot = Player(run, behaviour, seed)
        try:
            core.play(run, read, bot, ticks + 20000, on_tick=log, recorder=rec, trace=trace)
        finally:
            log.close()
            trace.close()
        print('tick', run.tick, 'notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:16], flush=True)
    with open(os.path.join(SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(rec.text())
    with open(os.path.join(SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)


def session_env(name):
    with open(os.path.join(SCRATCH, 'sessions', name + '.env')) as f:
        return json.load(f)


def key_moments(name, most):
    """Ticks worth a picture, from a session's samples: each new state, the top of the longest air, the best line."""
    moments, seen, air = {}, set(), 0
    best_air = best_line = 0
    path = os.path.join(SCRATCH, 'measure', name + '.samples.gz')
    with gzip.open(path, 'rt') as f:
        for line in f:
            r = json.loads(line)
            st = r.get('st', -1)
            if r['m'] != PLAY or not 0 <= st < 18:
                continue
            if st in (GRIND, MANUAL, AIR_QPIPE, AIR_SPINE, WALL_RIDE, VPOLE, VPOLE_A, HPOLE, SKITCH, VAULT, STALL) and st not in seen:
                seen.add(st)
                moments['first-' + STATES[st]] = r['t'] + 12
            air = air + 1 if st in AIRBORNE else 0
            if air == 20 and r['x'][1] < best_air:
                best_air = r['x'][1]
                moments['highest-air'] = r['t']
            line_worth = r['pend'] * max(1, r['mul'])
            if line_worth > best_line:
                best_line = line_worth
                moments['best-line'] = r['t']
    return dict(sorted(moments.items(), key=lambda kv: kv[1])[:most])


def shots(name, most):
    """The session replayed with the GS on, pictures at its key moments (a small set, in shots/)."""
    moments = key_moments(name, most)
    path = os.path.join(SCRATCH, 'sessions', name)
    script = open(path + '.script').read()
    end = max(moments.values()) + 5
    at = {t: label for label, t in moments.items()}
    env = dict(session_env(name), PS2X_HEADLESS='1')          # pictures without a window on anyone's screen
    with core.Run(config(name + '-shots', env, secs=max(500, end // 15), gs=True, script=script)) as run:
        def policy(s):
            due = [t for t in at if s.tick >= t]
            if due:
                label = at.pop(due[0])
                print(label, s.tick, STATES[s.st] if 0 <= s.st < 18 else s.st, run.shot('%s-%s' % (name, label)), flush=True)
            return core.NOTHING if s.tick < end else None
        core.play(run, read, policy, end + 4000)
    return moments


def replay(name, env=None, tag='replay'):
    """The session's pad script alone, the game read but not touched: the same trace, tick for tick?"""
    path = os.path.join(SCRATCH, 'sessions', name)
    script = open(path + '.script').read()
    end = max(int(i.split('v')[0]) + int(i.rsplit(':', 1)[1][:-1]) for i in script.split(','))
    out = '%s.%s.trace' % (path, tag)
    trace = core.Trace(out, every=60)
    with core.Run(config(name + '-' + tag, dict(session_env(name), **(env or {})), secs=max(300, end // 40), script=script)) as run:
        core.play(run, read, lambda s: core.NOTHING if run.script_time < end else None, end + 4000, trace=trace)
    trace.close()
    first = core.compare_traces(path + '.trace', out)
    if first and (first[1].startswith('end') or first[2].startswith('end')):      # the last line counts reads, and a live run answers a few past the script
        print(name, tag, 'to script time', end, 'SAME at all %d checkpoints' % (first[0] - 1))
        return None
    print(name, tag, 'to script time', end, 'SAME' if first is None else 'DIFFERS at line %d: %s | %s' % first)
    return first


# ---- measuring: what any input does, by the game's own numbers ----------------------------------

AIR_NAMES = ('Air', 'AirQPipe', 'AirSpine')


def samples(name):
    with gzip.open(os.path.join(SCRATCH, 'measure', name + '.samples.gz'), 'rt') as f:
        for line in f:
            yield json.loads(line)


def analyse(name):
    m = dict(ticks=0, fe=0, paused=0, skating=0, loading=0, dist=0.0, top=0.0, speedsum=0.0, stood=0, air=0, bestair=0,
             lands=0, bailland=0, crashes=0, combo=0, bestline=0.0, score=0, lines=0, tricks=0, linesbailed=0,
             tricksbailed=0, levels=set(), skaters=set(), states={}, entries={}, goals={}, opened=0, cc=0.0, rew=0)
    last, air, t0, line_mul, line_pend = None, 0, None, 0, 0.0
    crash0, lastt = {}, None
    for r in samples(name):
        dt = 1 if lastt is None else max(1, r["t"] - lastt)
        lastt = r["t"]
        m["ticks"] += dt
        if r["m"] == 1:
            m["fe"] += dt
        elif r["m"] == 3:
            m["paused"] += dt
        if r["m"] != 2 or "st" not in r or r["st"] >= len(STATES) or abs(r["x"][1]) > 15000:
            m["loading"] += dt if r["m"] == 2 else 0
            last = None
            continue
        if r.get("open") is not None:
            m["opened"] = max(m["opened"], bin(r["open"]).count("1"))
            for lv, g in enumerate(r["goals"]):
                m["goals"][(r["sk"], lv)] = g
            m["rew"] = max(m["rew"], sum(bin(v).count("1") for v in r["rew"]))
            m["cc"] = max(m["cc"], r["cc"])
        m["skating"] += dt
        if "j" in r and r["j"] >= 0:
            m["juicemin"] = min(m.get("juicemin", 1e9), r["j"])
            m["juicemax"] = max(m.get("juicemax", 0), r["j"])
        m["levels"].add(r["l"])
        m["skaters"].add(r["sk"])
        m["top"] = max(m["top"], r["sp"])
        m["speedsum"] += r["sp"] * dt
        m["stood"] += dt if r["sp"] < 0.5 else 0
        sn = STATES[r["st"]]
        m["states"][sn] = m["states"].get(sn, 0) + dt
        if sn in AIR_NAMES:
            air += dt
            m["air"] += dt
            m["bestair"] = max(m["bestair"], air)
        else:
            air = 0
        c = r["crash"]
        crash0.setdefault(r["p"], c)
        m["crashes"] = max(m["crashes"], c - crash0[r["p"]])
        m["score"] = max(m["score"], r.get("score", 0))
        mul, pend = r["mul"], r["pend"]
        if last:
            d = sum((a - b) ** 2 for a, b in zip(r["x"], last["x"])) ** 0.5
            if d < 6:
                m["dist"] += d
            if r["st"] != last["st"]:
                m["entries"][sn] = m["entries"].get(sn, 0) + 1
                if STATES[last["st"]] in AIR_NAMES and sn not in AIR_NAMES:
                    if r["fg"] & 2 or r["land"] >= 3:
                        m["bailland"] += 1
                    elif sn in ("Ground", "Manual", "Stand", "CessSlide"):
                        m["lands"] += 1
            # a line: pending points, then banked (the score rises) or lost (no rise)
            if last["pend"] > 0 and pend == 0:
                banked = r.get("score", 0) > last.get("score", 0)
                m["lines" if banked else "linesbailed"] += 1
                m["tricks" if banked else "tricksbailed"] += max(line_mul, 1)
                line_mul = 0
        line_mul = max(line_mul, mul if 0 <= mul < 1000 else 0)
        m["combo"] = max(m["combo"], line_mul)
        m["bestline"] = max(m["bestline"], pend * max(mul, 1) if mul < 1000 else 0)
        last = r
    m["goalsdone"] = sum(max(0, v) for v in m["goals"].values() if v < 1000)
    return m


def report(names):
    head = "input ticks fe_s pause_s skate_s dist top mean air_s bestair land bail crash lines tricks lost combo bestline score goals rewards open pct lvls skaters jmin jmax".split()
    print("| " + " | ".join(head) + " | states entered |")
    print("|---" * (len(head) + 1) + "|")
    for n in names:
        m = analyse(n)
        s = lambda t: "%.0f" % (t / 59.94)
        ent = " ".join("%s %d" % (k, v) for k, v in sorted(m["entries"].items(), key=lambda kv: -kv[1])
                       if k not in ("Ground", "Stand", "Air"))
        vals = [n, m["ticks"], s(m["fe"]), s(m["paused"]), s(m["skating"]), "%.0f" % m["dist"], "%.1f" % m["top"],
                "%.1f" % (m["speedsum"] / max(1, m["skating"])), s(m["air"]), "%.1f" % (m["bestair"] / 59.94), m["lands"],
                m["bailland"], m["crashes"], m["lines"], m["tricks"], m["tricksbailed"], m["combo"], "%.0f" % m["bestline"],
                m["score"], m["goalsdone"], m["rew"], m["opened"], "%.1f" % m["cc"], len(m["levels"]), len(m["skaters"]), "%.1f" % m.get("juicemin", -1), "%.1f" % m.get("juicemax", -1)]
        print("| " + " | ".join(str(v) for v in vals) + " | " + ent + " |")


def script_of(name):
    """An input as a script and its environment: career, logic, an ab_inputs name or a file."""
    tools = os.path.join(MAIN, 'tools')
    if name == 'career':
        return subprocess.check_output([sys.executable, os.path.join(tools, 'careercheck.py'), 'script'], cwd=MAIN, text=True).strip(), {}
    if name == 'logic':
        return subprocess.check_output([sys.executable, '-c', 'import logiccheck; print(logiccheck.native_script())'], cwd=tools, text=True).strip(), {}
    path = name if os.path.exists(name) else os.path.join(zpaths.ZAXIS_TOOLING, 'checks', 'ab_inputs', name + '.txt')
    script, env = '', {}
    with open(path) as f:
        for line in f:
            if line.startswith('#env '):
                key, _, value = line[5:].strip().partition('=')
                env[key] = value
            elif not line.startswith('#'):
                script += line.strip()
    return script, env


def measure(name, ticks):
    """An input replayed, the game read but not touched: samples in measure/NAME.samples.gz."""
    script, env = script_of(name)
    tag = os.path.basename(name).replace('.txt', '')
    log = Log(os.path.join(SCRATCH, 'measure', tag + '.samples.gz'))
    with core.Run(config('m-' + tag, env, secs=max(300, ticks // 40), script=script)) as run:
        try:
            core.play(run, read, lambda s: core.NOTHING if run.tick < ticks else None, ticks + 2000, on_tick=log)
        finally:
            log.close()


def main(argv):
    if argv and argv[0] == 'objectives':
        for k in (int(v) for v in argv[1:] or range(1, 8)):
            name = nav.LEVELS[k]
            lv = nav.Level(name)
            npcs = {p['name']: p for p in lv.points}

            def behaviour(bot):
                yield from boot(bot)
                yield from career(bot, k)
                print('== entry', k, name, 'level', bot.s.level, 'at', [round(v, 1) for v in bot.s.pos], 'juice', bot.s.juice,
                      bot.s.carton, 'open %#x' % bot.s.open_levels)
                for o in objectives(bot.run):
                    p = npcs.get(o['object'])
                    print(' ', o['index'], 'kind', o['kind'], 'pts', o['points'], o['state'], 'locked' if o['locked'] else '',
                          repr(o['title']), repr(o['text'][:110]), [round(v) for v in p['pos']] if p else 'no point %#x' % o['object'])

            with core.Run(config('obj%d' % k, {'PS2X_DEV_UNLOCK_ALL': '1'}, secs=120)) as run:
                core.play(run, read, Player(run, behaviour), 8000)
        return
    if argv and argv[0] == 'measure':
        measure(argv[1], int(argv[2]))
        return
    if argv and argv[0] == 'report':
        report(argv[1:])
        return
    if argv and argv[0] == 'shots':
        shots(argv[1], int(argv[2]) if len(argv) > 2 else 6)
        return
    if argv and argv[0] == 'replay':
        replay(argv[1])
        return
    if argv and argv[0] == 'career':      # career NAME TICKS_A_LEVEL SEED SKATER FIRST_ENTRY MOST_LEVELS
        w = argv[1:] + [None] * 6
        career_session(w[0], int(w[1]), int(w[2] or 1), int(w[3] or 0), int(w[4] or 1), int(w[5] or 8))
        return
    if argv and argv[0] == 'pe':      # pe NAME TICKS SEED THEME (the Park Editor: towers, a test run)
        w = argv[1:] + [None] * 3
        pe_session(w[0], int(w[1]), int(w[2] or 1), int(w[3] or 0))
        return
    if argv and argv[0] == 'mp':      # mp NAME GAME TICKS SEED ROUNDS (split screen; 4 Animal Rescue)
        w = argv[1:] + [None] * 4
        mp_session(w[0], int(w[1]), int(w[2]), int(w[3] or 1), int(w[4] or 1))
        return
    if argv and argv[0] == 'goals':      # goals NAME TICKS_A_VISIT SEED SKATER VISITS
        w = argv[1:] + [None] * 4
        goals_session(w[0], int(w[1]), int(w[2] or 1), int(w[3] or 0), int(w[4] or 3))
        return
    if argv and argv[0] == 'reload':      # reload NAME SAVES_DIR [PROFILE [ENTRY]]
        w = argv[1:] + [None] * 3
        reload_session(w[0], w[1], w[2] or None, int(w[3] or 1))
        return
    if argv and argv[0] == 'session':
        flags = [a for a in argv[1:] if a.startswith('--')]
        words = [a for a in argv[1:] if not a.startswith('--')]
        session(words[0], int(words[1]), words[2], int(words[3]), seed=int(words[4]) if len(words) > 4 else 1,
                skater=int(words[5]) if len(words) > 5 else 0, real='--real' in flags)
        return
    raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
