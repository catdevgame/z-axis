#!/usr/bin/env python3
"""BMX XXX (SLUS-20415): the player's table (RAM names and addresses) and how to play, on tools/playbot/core.py.

Rider offsets are DM2's (dm2.py) moved by the paired functions' memory instructions (bmxcorpus/tools/offsets.py),
each confirmed in a run. See bmxcorpus/progress.md.
"""
import dataclasses
import math
import os
import re
import struct
import sys

CORE = os.environ.get('PLAYBOT_CORE', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, CORE)
import core  # noqa: E402

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..'))  # (tooling/: zpaths.py)
import zpaths  # noqa: E402  (paths.env from the user's own home: a scratch HOME still finds the data)
SCRATCH = os.environ.get('BMXCORPUS', os.path.join(zpaths.ZAXIS_SCRATCH, 'bmxcorpus'))
RUNNER = os.environ.get('RUNNER', '')
ELF = os.environ.get('ELF', os.path.join(zpaths.PATHS.get('BMX_DATA', os.path.join(zpaths.ZAXIS_SCRATCH, 'zbind1', 'bmx')), 'disc', 'SLUS_204.15'))

MODE = 0x29D040          # 0 boot and movies, 1 front end, 2 loading and level, 3 paused
LEVEL_FRAME = 0x2A2DE0   # rider task frames since the level started; 0 in the front end
RIDER_TABLE = 0x2F3A30   # rider objects; [0] player 1
# The round (game/bmx/game/round.h RoundState, g_round): +0x00 flags, +0x04 the phase's clock in frames, +0x0C the
# phase (setRoundPhase's): 1 the game's start, 2 a turn's start, 3 the turn running, 4 the turn over, 5 the game over.
# A turn with a clock (SESSION, the park editor's: two minutes) ends at 0: the riders are put away (frozen where they
# were, the level's frame still counting) and the game over screen asks what next (PLAY AGAIN its first line).
ROUND, ROUND_FLAGS, ROUND_CLOCK, ROUND_PHASE = 0x2F35A0, 0x00, 0x04, 0x0C
TURN_RUNNING, TURN_OVER, GAME_OVER = 3, 4, 5
GAME_SCREEN = 0x2A2D70   # g_gameScreen (game/bmx/overlay/screens.h): 1 play, 22 the game over screen, 25 instructions
GAME_TYPE = 0x29EC57     # g_currentGameMode (u8, round.h): 0 challenge, 2 session, 3 free ride

# 9 and 10 are the nose wheelie and the manual, as in DM2 (a shot of physics 9 on DAM: the HUD's NOSE WHEELIE, bmxshared).
GROUND, AIR, BAIL, GRIND_APPROACH, GRIND, STALL_APPROACH, STALL, LIP_APPROACH, LIP, NOSE_WHEELIE, MANUAL = range(11)


@dataclasses.dataclass
class State:
    tick: int = 0
    script: int = 0
    mode: int = 0
    frame: int = 0
    rider: int = 0
    flags: int = 0
    action: int = 0
    action_frames: int = 0
    trick: int = 0xFF
    physics: int = 0
    pos: tuple = (0.0, 0.0, 0.0)
    vel: tuple = (0.0, 0.0, 0.0)
    fwd: tuple = (0.0, 0.0, 0.0)
    up: tuple = (0.0, -1.0, 0.0)
    balance: float = 0.0
    speed: float = 0.0
    run_score: float = 0.0
    line_score: int = 0
    grind_time: float = 0.0
    line_tricks: int = 0
    line_state: int = 0
    bail_kind: int = 0
    actions: tuple = (0, 0, 0, 0)
    counts: tuple = (0, 0, 0, 0, 0, 0, 0)
    session: int = 0
    round_phase: int = 0
    round_flags: int = 0
    clock: int = 0
    screen: int = 0
    game_type: int = 0
    digest: bytes = None

    @property
    def riding(self):
        return self.frame > 0 and self.mode == 2 and self.rider != 0

    @property
    def turn_over(self):
        """The turn is over (a session's time up): the rider is put away until the game over screen is answered."""
        return self.frame > 0 and self.round_phase in (TURN_OVER, GAME_OVER)


def read(run):
    m = run.mem
    head = m(MODE, 4) + m(LEVEL_FRAME, 4) + m(RIDER_TABLE, 4)
    mode, frame, rider = struct.unpack('<3I', head)
    s = State(tick=run.tick, script=run.script_time, mode=mode, frame=frame)
    s.digest = head
    if frame:
        r = m(ROUND, 0x10)
        s.round_flags, s.clock = struct.unpack_from('<2I', r, ROUND_FLAGS)
        s.round_phase = r[ROUND_PHASE]
        s.screen = struct.unpack('<I', m(GAME_SCREEN, 4))[0]
        s.game_type = m(GAME_TYPE, 1)[0]
    if frame and 0x100000 <= rider < 0x2000000:
        s.rider = rider
        a = m(rider + 0x170, 0x20)
        b = m(rider + 0x1C0, 0x10)      # run score, _, line score, line's grind time
        c = m(rider + 0x1BE0, 2)
        f = m(rider + 0x2600, 2)
        g = m(rider + 0x2620, 0xE0)     # up, fwd, _, pos, ... balance +0x58, vel +0xA0, speed +0xD8
        s.flags = struct.unpack_from('<I', a, 0)[0]
        s.action = a[0xF]
        s.actions = tuple(a[0xF:0x13])
        s.bail_kind = struct.unpack_from('<B', m(rider + 0x196, 1))[0]
        s.action_frames = struct.unpack_from('<H', a, 0x14)[0]
        s.trick = a[0x1E]
        s.run_score, _, s.line_score, s.grind_time = struct.unpack('<ffIf', b)
        s.line_state, s.line_tricks = c[0], c[1]
        s.physics = f[0]
        s.up = struct.unpack_from('<3f', g, 0)
        s.fwd = struct.unpack_from('<3f', g, 0x10)
        s.pos = struct.unpack_from('<3f', g, 0x30)
        s.balance = struct.unpack_from('<f', g, 0x58)[0]
        s.vel = struct.unpack_from('<3f', g, 0xA0)
        s.speed = struct.unpack_from('<f', g, 0xD8)[0]
        s.digest += a + b + c + f + g
    return s


def run_config(name, script, env=None, max_secs=900, gs=False, root=None):
    e = {'PS2X_READABLE': 'off', 'PS2X_MC_FD_BASE0': '1', 'PS2X_HEADLESS': '1', 'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'}
    e.update(env or {})
    return core.RunConfig(name=name, runner=RUNNER, elf=ELF, root=root or SCRATCH + '/runs', script=script, env=e,
                          max_secs=max_secs, gs=gs)


# The level's path men (the people who walk its paths; game/bmx/pathman/pathman.h PathManRecord, 0xCC bytes): a goal
# giver's record names its objective at +0x88 and holds it at +0xC8 (0: the level has none of the name); +0x70 the
# squared range it offers it in (at least 25), +0x7C the squared distance to the nearest rider this frame; +0x04 its
# follower, whose +0x20 is its place (game/zge/scene/follower.cpp Follower; a FastPathMan's, travel.fast 1 at +0x00,
# a fast follower, zge/scene/fast_paths.h, keeps its owner's position at the same +0x20).
PATHMAN_NOTICE, PATHMAN_RIDER_D2, PATHMAN_NAME, PATHMAN_OBJECTIVE = 0x70, 0x7C, 0x88, 0xC8
FOLLOWER_POSITION = 0x20


def path_men(run, names):
    """{record address: objective name} of the path men whose objective is one of names (RAM searched once)."""
    lo, hi = 0x100000, 0x2000000
    data = b''.join(run.mem(a, 0x100000) for a in range(lo, hi, 0x100000))
    out = {}
    for name in set(names):
        needle = name.encode('latin-1') + b'\0'
        i = -1
        while True:
            i = data.find(needle, i + 1)
            if i < 0:
                break
            rec = i - PATHMAN_NAME
            if rec % 4 or rec < 0:
                continue
            notice = struct.unpack_from('<f', data, rec + PATHMAN_NOTICE)[0]
            objective = struct.unpack_from('<I', data, rec + PATHMAN_OBJECTIVE)[0]
            fast, follower = struct.unpack_from('<iI', data, rec)
            if 25.0 <= notice <= 1e5 and fast in (0, 1) and lo <= follower < hi:
                out[lo + rec] = dict(name=name, objective=objective, fast=fast, follower=follower)
    return out


def path_man_now(run, rec, info):
    """(where it stands, the squared distance to the nearest rider) of a path man's record, as the game has them."""
    d2 = struct.unpack('<f', run.mem(rec + PATHMAN_RIDER_D2, 4))[0]
    pos = struct.unpack('<3f', run.mem(info['follower'] + FOLLOWER_POSITION, 12))
    return pos, d2


# An objective (game/zge/scene/objectives.h Objective): its prerequisites at +0xE28 (four, 0 for none) and its state at
# +0xE38 (ObjectiveState: under 2 not taken yet, 2 active, 3 in progress, 4 done, 5 complete). A giver's objective can
# be given once it is not taken yet and each prerequisite is complete (zge/scene/objectives.cpp; bmx objectives.cpp
# playerCanActivateObjective: and no timed objective of the player's runs): Dam's Drain waits for Dynamite, Patch
# for Drain (OBJCTV.TXT PREREQUISITE), so a TRIANGLE by Drain's giver before Dynamite is done gives nothing.
OBJECTIVE_PREREQUISITES, OBJECTIVE_STATE = 0xE28, 0xE38
OBJECTIVE_ACTIVE, OBJECTIVE_COMPLETE = 2, 5


def objective_state(run, objective):
    return run.u32(objective + OBJECTIVE_STATE) if objective else None


def objective_ready(run, objective):
    """Whether the game would give the objective now (bar a timed objective running): not taken, prerequisites complete."""
    if not objective:
        return False
    if objective_state(run, objective) >= OBJECTIVE_ACTIVE:
        return False
    pre = struct.unpack('<4I', run.mem(objective + OBJECTIVE_PREREQUISITES, 16))
    return all(p == 0 or objective_state(run, p) == OBJECTIVE_COMPLETE for p in pre)


DIALOG_TICKS = 120
SCREEN_GAME_OVER = 22     # g_gameScreen's GAME OVER in a single player run (ANOTHER RUN, SAVE GAME, ...; overlay/screens.h)
SCREEN_OBJECTIVES = 26    # its ANOTHER RUN: the next run's CHALLENGES list (FrameObjectives), CONTINUE to ride


def menu_script(level=0, rider=0, final=True):
    """Title, main menu, the level's cheat (the game's own: Extras, Cheats, the keyboard), HARDCORE TOUR, rider select,
    level select (the eight in their order, the cheat's open). Closed on MODE (a SYNC), so it replays at any speed."""
    import bmxroutes
    s = bmxroutes.Script()
    name, code = LEVELS[level]
    if code:
        bmxroutes.cheats(s, [code])
    s.press('CROSS', after=240)          # HARDCORE TOUR
    s.presses(*['RIGHT'] * rider, after=120)
    s.press('CROSS', after=600)          # rider (a rider's model takes its time)
    s.presses(*['RIGHT'] * level, after=120)
    if final:
        s.press('CROSS', after=300)      # level
    return s


def look(name, level, rider, after=900):
    """The way to a level with pictures, every `after` ticks past the menu script, for what a route ran into."""
    menu = menu_script(level, rider, final=False)
    n = {}

    def policy(st):
        k = (st.script - menu.t) // after
        if st.script >= menu.t and k != n.get('k'):
            n['k'] = k
            print(st.script, 'mode', st.mode, 'frame', st.frame, run.shot('s%d' % k), flush=True)
        return None if st.script > menu.t + 90 else core.NOTHING

    with core.Run(run_config(name, menu.text(), gs=True, max_secs=165)) as run:
        core.play(run, read, policy, max_ticks=menu.t + 20000)


def reach(level, rider):
    """Does the menu script reach the level? Null GS, no input past it."""
    menu = menu_script(level, rider)
    last = {}

    def policy(st):
        if st.mode != last.get('m') or (st.frame > 0) != last.get('f'):
            last['m'], last['f'] = st.mode, st.frame > 0
            print('level', level, 'rider', rider, 'script', st.script, 'of', menu.t, 'mode', st.mode, 'frame', st.frame, flush=True)
        return None if st.script > menu.t + 1800 else core.NOTHING

    with core.Run(run_config('reach%d%d' % (level, rider), menu.text(), max_secs=150)) as run:
        core.play(run, read, policy, max_ticks=menu.t + 20000)


def replay(name, suffix='replay', spec=None):
    """The session's script alone (no decisions): the same trace, tick for tick?"""
    import json
    import dm2
    d = session_dir(name)
    meta = json.load(open(os.path.join(d, 'meta.json')))
    script = open(os.path.join(d, 'script.txt')).read()
    # the session's own environment (its clock: PS2X_EE_SCALE), readable off unless a spec is given
    env = dict(meta.get('env', {}), PS2X_READABLE=spec or 'off')
    cfg = run_config(name + '-' + suffix, script, env=env, max_secs=int(os.environ.get('MAX_SECS', '420')))
    trace = core.Trace(os.path.join(d, 'trace-%s.txt' % suffix))
    meter = dm2.Meter()

    def policy(st):
        meter.tick(st)
        return None if st.script >= meta['end'] else core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=meta['end'] + 60000, trace=trace)
    trace.close()
    diff = core.compare_traces(os.path.join(d, 'trace.txt'), os.path.join(d, 'trace-%s.txt' % suffix))
    print(name, 'replay', 'IDENTICAL' if diff is None else 'DIFFERS at line %d: %s | %s' % diff, meter.s.row(dm2.COLUMNS), flush=True)


def states(name, spec, first, last, words=None):
    """As dm2.py states: the session's script under readable SPEC ('' the defaults), the rider's bytes (0x2800) and
    WORDS (ADDR[+N],...) at each script time in FIRST..LAST, into sessions/NAME/states-SPEC.json (dm2.py statesdiff
    with BMXCORPUS's sessions compares two)."""
    import json
    d = session_dir(name)
    meta = json.load(open(os.path.join(d, 'meta.json')))
    script = open(os.path.join(d, 'script.txt')).read()
    cfg = run_config(name + '-states-' + (spec or 'defaults'), script, env=dict(meta.get('env', {}), PS2X_READABLE=spec),
                     max_secs=int(os.environ.get('MAX_SECS', '900')))
    extra = []
    for w in filter(None, (words or '').split(',')):
        a, _, n = w.partition('+')
        extra.append((int(a, 16), int(n or '4', 0)))
    out = {}

    def policy(st):
        if first <= st.script <= last:
            row = dict(mode=st.mode, frame=st.frame, rider='%x' % st.rider)
            if st.rider:
                row['rider_hex'] = run.mem(st.rider, 0x2800).hex()
            for a, n in extra:
                row['%x' % a] = run.mem(a, n).hex()
            out[st.script] = row
        return None if st.script > last else core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=last + 60000)
    core.save_json(os.path.join(d, 'states-%s.json' % (spec or 'defaults')), out)
    print(name, spec or 'defaults', len(out), 'states')


def route(name, route_name, env=None):
    """A bmxroutes route's script alone (no decisions) to its end, in runs/NAME: what the run left (its saves folder's
    files, the [saves] lines)."""
    import dm2  # noqa: F401  (the dm2 tools on the path)
    import bmxroutes
    script = bmxroutes.script_of(route_name)
    end = max(int(i.split('v')[0]) for i in script.split(',')) + 600
    e = dict(bmxroutes.extra_env(route_name), PS2X_EE_SCALE='4096')
    e.update(env or {})
    cfg = run_config(name, script, env=e, max_secs=int(os.environ.get('MAX_SECS', '900')))
    with core.Run(cfg) as run:
        core.play(run, read, lambda st: None if st.script >= end else core.NOTHING, max_ticks=end + 60000)
    saves = os.path.join(run.dir, 'mc0-saves')
    files = sorted((os.path.relpath(os.path.join(r, f), saves), os.path.getsize(os.path.join(r, f)))
                   for r, _, fs in os.walk(saves) for f in fs) if os.path.isdir(saves) else []
    lines = [l.rstrip() for l in open(os.path.join(run.dir, 'run.log'), errors='replace') if l.startswith('[saves]')]
    print(name, route_name, 'end', run.script_time, 'saves', files, *lines, sep='\n  ')


def player_of(level, seed, log):
    """DM2's player with BMX's differences: the stick pulled back stops a rolling rider, so no flat combo of manuals;
    a rail's run-up is taken again from further back when the way there ends too close to the rail to hop on."""
    import player_dm2 as pd

    class Rail(pd.Rail):
        """DM2's rail run, with a run-up: the hop leaves the ground when the button is let go (frames after the press),
        so a run that starts nearer the rail than the hop's lead goes back along the rail's line first."""
        backed = False

        def step(self, st):
            if self.sub is None and not self.backed and self.frames == 0:
                a = self.pts[0]
                ahead = (a[0] - st.pos[0]) * self.t[0] + (a[2] - st.pos[2]) * self.t[1]
                if ahead < 12.0:
                    self.backed = True
                    back = (a[0] - self.t[0] * 24, a[1], a[2] - self.t[1] * 24)
                    if self.p.level.path(st.pos, back):
                        self.p.log(st, 'rail run-up from further back, ahead %.1f' % ahead)
                        self.sub = pd.GoTo(self.p, back, near=4.0)
                        self.hop = 0
            return super().step(st)

    class Giver(pd.Task):
        """To a goal giver (bmxgoals.givers: a person who holds a goal's name), then the game's own way to take a
        challenge (its help text, STRING.ZAL: "Press the TRIANGLE BUTTON when you are near a character to get their
        challenge"): the last stretch at a walking pace, the rider stopped beside them (the stick pulled back),
        TRIANGLE pressed, and pressed again if nothing changed; the session's policy then answers the dialog (CROSS
        while the rider is not riding)."""
        name = 'giver'
        limit = 1500
        BESIDE = 2.2        # (the people's records hold 5.0: their reach, as read)
        PRESSES = 3

        def __init__(self, player, giver):
            super().__init__(player)
            self.giver = giver
            self.sub = pd.GoTo(player, giver['pos'], near=10.0)
            self.creep = 0      # frames of the last stretch
            self.wait = 0       # frames stood beside the giver
            self.presses = 0
            self.sought = False # went straight at them once the paths ended short (GIVER_SEEK)

        @property
        def standing(self):
            # Beside the giver the rider stands still on purpose: not stuck (Player.ride's count of slow frames).
            return self.sub is None

        def live(self, st):
            """The giver as the game has it (path_men): where it stands now (path men walk their paths) and how near the
            game finds the rider; the data point's place and no distance when no path man has the goal's name."""
            run = getattr(self.p, 'run', None)
            if run is None:
                return self.giver['pos'], None
            self.p.path_men(st)
            if getattr(self, 'record', None) is None:
                mine = [(k, v) for k, v in self.p.pathmen.items() if v['name'] == self.giver['goal']]
                best = None
                for k, v in mine:
                    pos, _ = path_man_now(run, k, v)
                    if pos and (best is None or core.dist2(pd.xz(pos), pd.xz(self.giver['pos'])) < best[0]):
                        best = (core.dist2(pd.xz(pos), pd.xz(self.giver['pos'])), k, v)
                self.record = best[1:] if best else ()
                if best:
                    self.p.log(st, 'giver %s: path man %#x, %.1f from its data point' % (self.giver['goal'], best[1], best[0] ** 0.5))
            if not self.record:
                return self.giver['pos'], None
            pos, d2 = path_man_now(run, *self.record)
            return pos or self.giver['pos'], d2

        def step(self, st):
            if self.sub and self.p.goal_running:
                return None               # a goal runs: its tricks first, not the way to the next giver
            where, d2 = self.live(st)
            if self.sub and getattr(self, 'record', None) and core.dist2(pd.xz(where), pd.xz(self.sub.goal)) > 16.0:
                self.sub = pd.GoTo(self.p, where, near=10.0)      # it walked on: after it
            if self.sub:
                self.frames += 1
                if self.frames > self.limit:
                    return None
                r = self.sub.step(st)
                if r is not None:
                    return r
                self.sub = None
                d = core.dist2(pd.xz(st.pos), pd.xz(where)) ** 0.5
                self.p.log(st, 'giver %s (%s) at %.1f%s' % (self.giver['goal'], self.giver['model'], d,
                                                            '' if d2 is None else ', the game\'s %.1f' % d2 ** 0.5))
                if d > 12:
                    if os.environ.get('GIVER_SEEK') == '1' and not self.sought and d < 80:
                        # (panel2) a giver off the level's paths (the Bronx's hookers stand on the sidewalks): straight at them
                        self.sought = True
                        self.sub = Seek(self.p, self.giver['pos'], near=8.0)
                        return self.sub.step(st)
                    return None
            d = core.dist2(pd.xz(st.pos), pd.xz(where)) ** 0.5
            if self.wait == 0 and d > self.BESIDE:
                # The last stretch at a walking pace: towards them, pedalling only below it, braking above.
                self.creep += 1
                if self.creep > 600:
                    self.p.log(st, 'giver: not reached, %.1f away' % d)
                    return None
                lx, _ = pd.steer(st, where)
                if st.speed > 0.10:
                    return core.pad(lx=lx, ly=1.0)
                return core.pad('UP', lx=lx) if st.speed < 0.05 else core.pad(lx=lx)
            self.wait += 1
            if self.wait == 1:
                self.p.log(st, 'giver: beside, %.1f away' % d)
            first, every = 60, 150
            if self.presses < self.PRESSES:
                if st.speed > 0.01 and self.presses == 0:
                    return core.pad(lx=0.0, ly=1.0)          # stop
                k = self.wait - first
                if k >= 0 and k % every == 0:
                    self.presses += 1
                    run = getattr(self.p, 'run', None)
                    flags = run.mem(st.rider + 0x178, 6).hex() if run and st.rider else '?'
                    self.p.log(st, 'giver: TRIANGLE %d, %.1f away%s, rider flags %s' % (
                        self.presses, d, '' if d2 is None else ' (the game\'s %.1f)' % d2 ** 0.5, flags))
                    self.p.wants.append('%s-%d' % (self.giver['goal'], self.presses))
                    self.p.goal_watch = True
                if k >= 0 and k % every < 6:
                    return core.pad('TRIANGLE')
                return core.NOTHING
            # The challenge's cutscene and its worker's lines hold the rider: CROSS shows the next line (TRIANGLE does
            # not); pedal between, until the rider rolls again (the goal runs).
            if st.speed > 0.05:
                self.p.log(st, 'giver: riding again, %d frames after the first TRIANGLE' % (self.wait - first))
                self.p.giver_done = st.frame
                items = self.p.collect_items.get(self.giver['goal'], [])
                if items:
                    # A COLLECT goal: its things, in OBJCTV.TXT's order (the checkpoint last), ridden through next.
                    self.p.collect = list(items)
                    if os.environ.get('COLLECT_ORDER') == 'near':
                        # (a COLLECT with no order: any thing first) the nearest next, each time from the last
                        left, at, order = list(items), pd.xz(st.pos), []
                        while left:
                            nxt = min(left, key=lambda q: core.dist2(at, pd.xz(q)))
                            left.remove(nxt)
                            order.append(nxt)
                            at = pd.xz(nxt)
                        self.p.collect = order
                    self.p.log(st, 'collect: %d things for %s' % (len(items), self.giver['goal']))
                return None
            if self.wait > first + every * self.PRESSES + 1200:
                self.p.log(st, 'giver: still held after %d frames' % (self.wait - first))
                return None
            if self.wait % 90 < 4:
                return core.pad('CROSS')
            return core.pad('UP') if self.wait % 90 > 10 else core.NOTHING

    class Seek(pd.Task):
        """Straight at a point (a goal's thing off the level's paths): steered and pedalled, done within `near`."""
        name = 'seek'
        limit = 900

        def __init__(self, player, goal, near=2.0):
            super().__init__(player)
            self.goal, self.near = goal, near

        def step(self, st):
            self.frames += 1
            if self.frames > self.limit or core.dist2(pd.xz(st.pos), pd.xz(self.goal)) ** 0.5 < self.near:
                return None
            lx, _ = pd.steer(st, self.goal)
            d = core.dist2(pd.xz(st.pos), pd.xz(self.goal)) ** 0.5
            if d < 6.0:
                # the approach's numbers: the thing's height against the rider's (y grows down), the hop's apex
                self.apex = min(getattr(self, 'apex', 1e9), st.pos[1])
                if d < getattr(self, 'closest', 1e9):
                    self.closest = d
                    self.p.log(st, 'seek: %.2f away (x/z), thing y %.2f, rider y %.2f, apex so far %.2f, physics %d'
                               % (d, self.goal[1], st.pos[1], self.apex, st.physics))
            if d < 6.0 and self.goal[1] < st.pos[1] - 1.5 and self.frames % 40 < 8:
                return core.pad('UP', 'CROSS', lx=lx)     # the thing hangs above the ground (y grows down): hop through it
            return core.pad('UP', lx=lx)

    class Flatland(pd.Task):
        """(routes2) BMX's flatland (action state 6), found by test programs (bmx.py flat): from a straight run at speed,
        the manual's two stick taps (up, then down), then trick A (SQUARE) held with the d-pad tapped left and right: the
        flatland's trick; held again with more taps for a chained one; let go, the rider rides out (or bails)."""
        name = 'flatland'
        limit = 420

        def __init__(self, player, target):
            super().__init__(player)
            self.target = target
            self.prog = []
            self.phase = 0
            self.seen = set()

        def step(self, st):
            self.frames += 1
            if st.action == 6 and 6 not in self.seen:
                self.p.log(st, 'flatland: action 6, sub-state %d' % st.actions[1])
            self.seen.add(st.action)
            if self.frames > self.limit or st.physics in (pd.AIR, pd.BAIL) and self.phase > 0:
                if self.phase > 0:
                    self.p.log(st, 'flatland over: actions seen %s' % sorted(self.seen))
                    self.p.flatlands += 6 in self.seen
                return None
            if self.prog:
                return self.prog.pop(0)
            lx, err = pd.steer(st, self.target, gain=1.2)
            if self.phase == 0:
                if abs(err) > 1.2 and self.frames > 40:
                    return None
                if st.speed > 0.22 and abs(err) < 0.3:
                    self.phase = 1
                    U, D, N = core.pad(ly=-1.0), core.pad(ly=1.0), core.NOTHING
                    self.prog = [U] * 3 + [N] * 3 + [D] * 3 + [N] * 3 + [core.pad(ly=0.3)] * 20
                    return self.prog.pop(0)
                return core.pad('UP', lx=lx)
            if self.phase in (1, 2, 3) and st.physics in (pd.MANUAL, pd.GROUND):
                self.phase += 1
                S = 'SQUARE'
                self.prog = [core.pad(S, 'LEFT')] * 4 + [core.pad(S)] * 3 + [core.pad(S, 'RIGHT')] * 4 + [core.pad(S)] * 3 + \
                            [core.NOTHING] * (40 if self.phase < 4 else 120)
                return self.prog.pop(0)
            return core.NOTHING

    class WallTap(pd.Task):
        """(bmxmp's HIGHEST WALLTAP) To a tall wall of the level's map (nav.wall_segment's: 7 long, 3 high at least), a
        run in closing on it all along it, the hop beside it, and in the air the stall move (the hop's button, CROSS,
        pressed again) with the stick to the right: the lip trick of a lip face (a face 75 degrees or more from flat,
        approachEnter's search) is g_lipTricks' by the stick's direction, right's 0x46 the wall tap, whose height over
        the ground is the record DISTANCE_TAP (countLandedTrick; up and down give 0x47, the wall ride's plant)."""
        name = 'walltap'
        limit = 1500
        RUN = 12.0          # the run in starts this far back from the wall's near end, OFF out from it, and aims at its
        OFF = 4.0           # far end AIM out: the rider closes on the wall all along it
        AIM = 1.2
        HOP_AT = 2.2        # the hop goes when the rider is this near the wall's plane
        HOP_HOLD = 16       # frames the hop's button is held (let go: the hop)

        def __init__(self, player, way):
            super().__init__(player)
            self.way = way
            self.sub = pd.GoTo(player, way['start'], near=4.0)
            self.hop = 0
            self.air_frames = 0
            self.pressed = 0

        def off(self, st):
            m, n = self.way['mid'], self.way['normal']
            return (st.pos[0] - m[0]) * n[0] + (st.pos[2] - m[2]) * n[1]

        def step(self, st):
            self.frames += 1
            if self.frames > self.limit:
                return None
            if self.sub:
                r = self.sub.step(st)
                if r is not None:
                    return r
                self.sub = None
                if core.dist2(pd.xz(st.pos), pd.xz(self.way['start'])) > 64:
                    self.p.log(st, 'walltap: run-up not reached')
                    return None
                self.p.log(st, 'walltap run, wall %.0f long %.1f high' % (self.way['length'], self.way['height']))
                self.frames = 0
            if self.hop and st.physics != pd.GROUND:
                return None            # (the air is the player's: air())
            m, n, t = self.way['mid'], self.way['normal'], self.way['tangent']
            half = self.way['length'] / 2
            aim = (m[0] + n[0] * self.AIM + t[0] * half, m[1], m[2] + n[1] * self.AIM + t[1] * half)
            lx, err = pd.steer(st, aim, gain=1.8)
            along = (st.pos[0] - m[0]) * t[0] + (st.pos[2] - m[2]) * t[1]
            if self.frames > 400 or along > self.way['length'] / 2 or self.off(st) < -0.5:
                self.p.log(st, 'walltap run ends: off %.1f along %.1f hop %d' % (self.off(st), along, self.hop))
                return None
            inside = abs(along) < self.way['length'] / 2 - 1.0     # beside the wall itself, not its line's extension
            if self.hop == 0 and inside and 0.0 < self.off(st) < self.HOP_AT and st.speed > 0.1:
                self.hop = 1
                self.p.log(st, 'walltap hop, off %.1f along %.1f' % (self.off(st), along))
            if self.hop and self.hop < self.HOP_HOLD:
                self.hop += 1
                return pd.pad('UP', pd.HOP, lx=lx)
            if self.hop:
                self.hop += 1
                if self.hop > self.HOP_HOLD + 30:
                    return None        # no air came of it
            return pd.pad('UP', lx=lx)

        def air(self, st):
            """In the air beside the wall: the stall move (CROSS pressed: let go a frame, then held) with the stick right,
            asked again every 12 frames while the wall is near."""
            self.air_frames += 1
            k = self.air_frames
            if abs(self.off(st)) > 3.5 or k < 3:
                return core.NOTHING
            if (k - 3) % 12 == 0:
                self.pressed += 1
                self.p.log(st, 'walltap ask %d, off %.1f, height over the floor %.2f' % (self.pressed, self.off(st), self.way['mid'][1] - st.pos[1]))
            return pd.pad(pd.HOP, lx=0.95) if (k - 3) % 12 < 8 else pd.pad(lx=0.95)

    class Player(pd.Player):
        contest = None      # (bmxmp) a three-chance contest's turns: 'grind' (LONGEST GRIND), 'walltap' (HIGHEST WALLTAP)
        givers = []
        flatlands = 0
        collect_items = {}  # goal -> the positions its COLLECT objectives ask for (bmxgoals.collect_items)
        collect = []        # what is left to ride through of the goal taken
        collect_tries = 0
        wants = []          # moments a task asks to keep: pictures with gs, RAM with DUMPS (the session's policy)
        goal_watch = False  # a goal was asked for: the session's policy reads the goals' HUD text from then on
        goal_running = False  # a goal's HUD line is up (the policy's reading)
        giver_done = -10000   # the level frame a giver's challenge let the rider go (its HUD line comes a little later)

        perched = 0         # frames in the air state at a standstill
        pathmen = None      # the givers' path men (path_men), found once
        pathmen_at = 0
        giver_tries = {}    # id(giver) -> times taken to it

        def path_men(self, st):
            run = getattr(self, 'run', None)
            if run is not None and (self.pathmen is None or (not self.pathmen and st.frame - self.pathmen_at > 600)):
                self.pathmen, self.pathmen_at = path_men(run, [g['goal'] for g in self.givers]), st.frame   # (again later if none yet)
                self.log(st, 'path men: %s' % (', '.join('%s %#x (objective %#x)' % (v['name'], k, v['objective'])
                                                         for k, v in sorted(self.pathmen.items())) or 'none'))
            return self.pathmen or {}

        def giver_ready(self, st, g):
            """The giver's objective as the game has it: ready to give (objective_ready), else (taken, complete, or
            waiting for a prerequisite) not now. A giver with no path man of its name: as before, once."""
            run = getattr(self, 'run', None)
            objs = {v['objective'] for v in self.path_men(st).values() if v['name'] == g['goal']}
            if run is None or not objs:
                return id(g) not in self.visited
            return any(objective_ready(run, o) for o in objs)

        def __call__(self, st):
            # The air state at a standstill: BMX's rider can come to rest on a ledge's edge still in the air state (DAM,
            # rails only, seed 21: a lip trick's landing at f1560, physics 1 at speed 0.004 for 1,500 frames), and DM2's
            # player only judges being stuck on the ground (ride). What a player does: pedal and hop off, then the bail
            # button (all four shoulders), as for a rider wedged on the ground.
            if isinstance(self.task, Flatland) and self.task.phase > 0 and st.physics in (pd.GROUND, pd.MANUAL, pd.NOSE_WHEELIE):
                # (the flatland's manual and its trick are the task's: DM2's player balances a manual by itself)
                r = self.task.step(st)
                if r is not None:
                    return r
                self.task = None
            if isinstance(self.task, WallTap) and st.physics == pd.AIR and self.task.hop:
                return self.task.air(st)
            self.perched = self.perched + 1 if st.physics == pd.AIR and st.speed < 0.02 else 0
            if self.perched > 60:
                k = self.perched - 60
                if k == 1:
                    self.log(st, 'perched in the air')
                    self.task = None
                if k < 90:
                    return pd.pad(pd.HOP, 'UP') if k % 30 < 12 else pd.pad('UP', lx=0.95)
                if k < 98:
                    return pd.pad('L1', 'R1', 'L2', 'R2')
                self.log(st, 'perched: still after the hops and the bail button')
                self.perched = 0
            return super().__call__(st)

        def new_turn(self, st):
            """A turn starts over at the level's start (bmxmp): the task and the rails tried dropped."""
            self.task = None
            self.visited = set()
            self.log(st, 'turn: planned again from here')

        def wall_ways(self):
            """The level map's tall walls a run in along reaches (WallTap): each wall's foot at its middle with a floor
            beside it at the wall's bottom, the run's start RUN back from its near end and OFF out, on the floor at about that
            height, and floor all the way between (no drop, no step up)."""
            if getattr(self, '_wall_ways', None) is not None:
                return self._wall_ways
            out = []
            for x0, z0, x1, z1, top, bottom, nx, nz in getattr(self.level, 'walls', []):
                length = math.hypot(x1 - x0, z1 - z0)
                t = ((x1 - x0) / length, (z1 - z0) / length)
                for sign in (1, -1):
                    tt = (t[0] * sign, t[1] * sign)
                    mid = ((x0 + x1) / 2, bottom, (z0 + z1) / 2)
                    foot = self.level.nearest((mid[0] + nx * 1.5, bottom, mid[2] + nz * 1.5), radius=1)
                    if not foot or abs(self.level.world(foot)[1] - bottom) > 1.0:
                        break       # no floor at the wall's foot on this side
                    floor = self.level.world(foot)[1]
                    back = length / 2 + WallTap.RUN
                    start = (mid[0] + nx * WallTap.OFF - tt[0] * back, floor, mid[2] + nz * WallTap.OFF - tt[1] * back)
                    end = (mid[0] + nx * WallTap.AIM + tt[0] * length / 2, floor, mid[2] + nz * WallTap.AIM + tt[1] * length / 2)
                    ok = True
                    for k in range(13):
                        f = k / 12.0
                        q = (start[0] + (end[0] - start[0]) * f, floor, start[2] + (end[2] - start[2]) * f)
                        node = self.level.nearest(q, radius=1)
                        if not node or abs(self.level.world(node)[1] - floor) > 0.6:
                            ok = False
                            break
                    node = self.level.nearest(start, radius=1)
                    if ok and node:
                        out.append(dict(start=self.level.world(node), mid=(mid[0], floor, mid[2]), normal=(nx, nz), tangent=tt,
                                        length=length, height=bottom - top, part=self.level.part.get(node)))
            self._wall_ways = out
            self.log.f.write('wall ways %d\n' % len(out))
            return out

        def contest_task(self, st, rails):
            """The contest's feature, the nearest first (straight line), each tried once before any again."""
            here = pd.xz(st.pos)
            part = self.level.main_part(st.pos)
            if self.contest == 'grind':
                ways = [r for r in rails]
                make = Rail
            else:
                ways = [w for w in self.wall_ways() if w['part'] == part]
                make = WallTap
            if not ways:
                return None
            way = min(ways, key=lambda w: core.dist2(here, pd.xz(w['start'])) + (1e6 if id(w) in self.visited else 0))
            self.visited.add(id(way))
            return make(self, way)

        def ride(self, st):
            # A task standing still on purpose (the giver's) is not stuck: DM2's player gives up a task after 40 slow
            # frames, which ended the giver's about 80 frames after arriving, before its TRIANGLE (play4).
            if getattr(self.task, 'standing', False):
                self.slow = 0
            return super().ride(st)

        def reachable(self, p):
            node = self.level.nearest(p)
            if not node:
                return False
            part = self.level.part.get(node)
            return part is not None and self.level.sizes[part] > 1000

        def rail_entry(self, pts):
            """DM2's way onto a rail, kept only when the run-up's ground stays a hop below the rail all the way to it:
            a run-up that drops off a ledge on the way left the rider more than 3 below the rail (rise > 3) and the
            run ended at once."""
            e = super().rail_entry(pts)
            if not e:
                return None
            a, (sx, sy, sz) = pts[0], e['start']
            y = sy
            for k in range(1, 9):
                f = k / 8.0
                p = (sx + (a[0] - sx) * f, y, sz + (a[2] - sz) * f)
                node = self.level.nearest(p, radius=1)
                if not node:
                    return None
                y2 = self.level.world(node)[1]
                if abs(y2 - y) > 1.2 or not (-0.6 <= y2 - a[1] <= 2.4):
                    return None
                y = y2
            return e

        def next_task(self, st):
            rng = self.rng
            here = pd.xz(st.pos)
            # Only what the rider's part of the level reaches (elsewhere there is no path: the task would end at once).
            part = self.level.main_part(st.pos)
            rails = [r for r in self.rails if r['part'] == part]
            ramps = [f for f in self.ramps if f['part'] == part]
            if self.contest:
                task = self.contest_task(st, rails)
                if task:
                    return task
            near_rails = sorted(rails, key=lambda r: core.dist2(here, pd.xz(r['start'])) + rng.random() * 900 + (4e4 if id(r) in self.visited else 0))[:6]
            near_ramps = sorted(rng.sample(ramps, min(120, len(ramps))), key=lambda f: core.dist2(here, pd.xz(f['centre'])) + rng.random() * 900)
            givers = [g for g in self.givers if self.giver_tries.get(id(g), 0) < 3 and self.level.main_part(g['pos']) == part
                      and self.giver_ready(st, g)]
            if os.environ.get('GOAL_FIRST') and not getattr(self, 'first_done', False):
                # one giver's goal before any other (another goal running holds the next giver's back)
                givers = [g for g in givers if g['goal'] == os.environ['GOAL_FIRST']]
                if givers:
                    self.first_done = True
            if self.collect:
                # The goal's next thing: through it (GoTo's near), again if missed, at most three tries each.
                target = self.collect[0]
                if core.dist2(here, pd.xz(target)) ** 0.5 < 2.5 or self.collect_tries >= 3:
                    self.log(st, 'collect: %s %s, %.1f away' % ('reached' if self.collect_tries < 3 else 'given up',
                                                               '(%.0f %.0f %.0f)' % target, core.dist2(here, pd.xz(target)) ** 0.5))
                    self.collect.pop(0)
                    self.collect_tries = 0
                    if not self.collect:
                        return pd.Ramp(self, near_ramps[0]) if near_ramps else None
                    target = self.collect[0]
                self.collect_tries += 1
                # by the level's paths when there is one, else straight at it (a GoTo with no path ends at once)
                # (the paths to within 5, then straight through it: a thing above the ground is hopped through)
                path = self.level.path(st.pos, target) if self.collect_tries == 1 and core.dist2(here, pd.xz(target)) > 36 else None
                return pd.GoTo(self, target, near=5.0) if path else Seek(self, target, near=1.0)
            if self.goal_running or st.frame - self.giver_done < 240:
                # A goal runs (its HUD line is up): tricks for it, off the ramps near and on rails, not the next giver.
                if near_rails and rng.random() < 0.3:
                    return Rail(self, near_rails[0])
                if near_ramps:
                    return pd.Ramp(self, near_ramps[0])
            if givers:
                # (the givers of COLLECT goals first: their things are places, a goal the player can finish)
                first = os.environ.get('GOAL_FIRST', '')
                g = min(givers, key=lambda g: (g['goal'] != first, g['goal'] not in self.collect_items, core.dist2(here, pd.xz(g['pos']))))
                self.visited.add(id(g))
                self.giver_tries[id(g)] = self.giver_tries.get(id(g), 0) + 1
                return Giver(self, g)
            if os.environ.get('FLATLAND') == '1' and rng.random() < 0.35:
                far = self.far_cell(st)
                if far:
                    return Flatland(self, far)
            k = 0.0 if self.rail_only else rng.random()
            if k < 0.5 and near_rails:
                self.visited.add(id(near_rails[0]))
                return Rail(self, near_rails[0])
            if k < 0.85 and near_ramps:
                return pd.Ramp(self, near_ramps[0])
            far = self.far_cell(st)
            if far:
                return pd.Explore(self, far, near=8.0)
            return pd.Ramp(self, near_ramps[0]) if near_ramps else None

    pl = Player(level, seed, log)
    if os.environ.get('GOALS') == '1':
        import bmxgoals
        pl.givers = bmxgoals.givers(level.name)
        pl.collect_items = bmxgoals.collect_items(level.name)
        log.f.write('givers %d\n' % len(pl.givers))
    return pl


def presses(*items, hold=6, gap=34):
    """A menu program: each item a button (pressed `hold` frames, then `gap` frames of nothing) or an int (frames of
    nothing)."""
    out = []
    for it in items:
        if isinstance(it, int):
            out += [core.NOTHING] * it
        else:
            out += [core.pad(it)] * hold + [core.NOTHING] * gap
    return out


# The pause menu's screens (routes2), as a player visits them between rides: PAUSED (CONTINUE, VIEW CHALLENGES, VIEW BIKE
# PARTS, VIEW RECORDS, OPTIONS, END RUN; the cursor on CONTINUE), OPTIONS (CAMERA TYPE: BEHIND, ABOVE, CLOSE, CHASE;
# VIBRATION; SFX VOLUME; MUSIC VOLUME; MUSIC TRACK). Each program starts and ends riding.
def interlude_screens():
    """VIEW CHALLENGES and a challenge's DETAILS (CIRCLE), VIEW BIKE PARTS, VIEW RECORDS (its pages); CONTINUE (the
    cursor kept on CONTINUE for the next pause: the menu remembers its place)."""
    return presses('START', 60, 'DOWN', 'CROSS', 150, 'CIRCLE', 120, 'TRIANGLE', 90, 'TRIANGLE', 90, 'DOWN', 'CROSS', 150,
                   'TRIANGLE', 60, 'DOWN', 'CROSS', 150, 'RIGHT', 60, 'DOWN', 60, 'TRIANGLE', 60, 'UP', 'UP', 'UP', 'CROSS', 90)


def interlude_options(track=1):
    """OPTIONS: the next camera type, vibration toggled, the volumes nudged, `track` music tracks on."""
    return presses('START', 60, 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'CROSS', 120, 'RIGHT', 40, 'DOWN', 'RIGHT', 40,
                   'DOWN', 'LEFT', 'RIGHT', 'DOWN', 'LEFT', 'RIGHT', 'DOWN', *(['RIGHT', 60] * track), 'LEFT', 60,
                   'TRIANGLE', 60, 'UP', 'UP', 'UP', 'UP', 'CROSS', 90)


def interlude_menu():
    """OPTIONS walked as a list (cov5): UP on its first line (blocked or wrapped), DOWN past its last, UP back, CROSS on the
    camera line, CIRCLE, then TRIANGLE back to PAUSED and CONTINUE (as interlude_options)."""
    return presses('START', 60, 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'CROSS', 120, 'UP', 40, 'UP', 40, 'DOWN', 'DOWN', 'DOWN',
                   'DOWN', 'DOWN', 'DOWN', 40, 'UP', 'UP', 'UP', 'UP', 'UP', 'UP', 40, 'CROSS', 60, 'CIRCLE', 60,
                   'TRIANGLE', 60, 'UP', 'UP', 'UP', 'UP', 'CROSS', 90)


def interludes(spec):
    """INTERLUDES=T:KIND,...: the programs (KIND screens, options or menu), each T ticks of play after the last one ended (the
    first T after the start)."""
    out = []
    for item in (spec or '').split(','):
        if item:
            t, kind = item.split(':')
            out.append((int(t), interlude_screens() if kind == 'screens' else interlude_menu() if kind == 'menu' else interlude_options(2)))
    return sorted(out, key=lambda x: x[0])


def session_dir(name):
    return os.path.join(SCRATCH, 'sessions', name)


def play_session(name, level=0, rider=0, seed=1, ticks=5400, gs=False, max_secs=420, policy_name='player', env=None):
    """One played run from the level's start. sessions/NAME: script.txt (the replay), trace.txt, stats.json, play.log."""
    import dm2
    import bmxmap  # noqa: F401
    import dm2map
    import player_dm2
    menu = menu_script(level, rider)
    d = session_dir(name)
    os.makedirs(d, exist_ok=True)
    # (routes2) the guest clock fixed, so the recording of the session's script replays the play: PLAY_ENV adds more
    env = dict(env or {})
    env.setdefault('PS2X_EE_SCALE', '4096')
    for kv in filter(None, os.environ.get('PLAY_ENV', '').split(';')):
        k, v = kv.split('=', 1)
        env[k] = v
    cfg = run_config(name, menu.text(), env=env, max_secs=max_secs, gs=gs)
    meter, log = dm2.Meter(), player_dm2.Log(os.path.join(d, 'play.log'))
    lv = dm2map.Level(LEVELS[level][0])
    pl = player_of(lv, seed, log)
    rec = core.Recorder()
    trace = core.Trace(os.path.join(d, 'trace.txt'))
    start = {}

    shot_every = int(os.environ.get('SHOT_EVERY', '0'))    # with gs: a picture every so many ticks of play
    dumps = int(os.environ.get('DUMPS', '0'))              # RAM dumps kept at the moments a task asks for, at most
    dump_every = int(os.environ.get('DUMP_EVERY', '0'))    # and one every so many ticks of play (the panel's shots of a run)
    seen = {}
    pending = []

    def keep(st):
        # A task's moment (the giver's TRIANGLE): a picture (gs) and the RAM (DUMPS) then and after it.
        if dump_every and st.riding and st.script % dump_every == 0:
            pending.append((st.script, 'tick-%d' % st.script))
        while pl.wants:
            what = pl.wants.pop(0)
            for after in (0, 40, 150, 420):
                pending.append((st.script + after, '%s+%d' % (what, after)))
        for at, what in [w for w in pending if w[0] <= st.script]:
            pending.remove((at, what))
            if gs:
                log(st, 'shot', run.shot(what))
            if dumps > 0:
                dumps_left = len([n for n in os.listdir(d) if n.endswith('.ram')])
                if dumps_left < dumps:
                    with open(os.path.join(d, what + '.ram'), 'wb') as f:
                        f.write(run.mem(0, 0x2000000))
                    log(st, 'ram', what)

    pending_programs = interludes(os.environ.get('INTERLUDES'))
    program = []
    played_programs = []

    goal_text = re.compile(rb'(Gaps found|Tanks destroyed|Switches hit|Potty points|Cracks patched|TNT found|[A-Z][a-z]+ [a-z]+): (\d+) of (\d+)\x00')
    goals = {}

    def watch_goals(st):
        # The goals' HUD lines as the game formats them ("Potty points: 4200 of 30000"), every 2 seconds once one was asked for.
        if not pl.goal_watch or st.script % 120:
            return
        ram = run.mem(0, 0x2000000)
        at = {m.start(): (m.group(1).decode(), int(m.group(2)), int(m.group(3))) for m in goal_text.finditer(ram)}
        # (texts at the places they held at the first look are the game's tables, not a goal's count: left out)
        before = goals.setdefault('before', {a: t for a, t in at.items()})
        found = sorted(set(t for a, t in at.items() if before.get(a) != t))
        pl.goal_running = bool(found)
        if found != goals.get('last'):
            goals['last'] = found
            log(st, 'goal text', '; '.join('%s: %d of %d' % f for f in found) or 'none')
            for f in found:
                goals.setdefault('best', {})[f[0] + ' of %d' % f[2]] = max(f[1], goals.get('best', {}).get(f[0] + ' of %d' % f[2], 0))

    save_end = os.environ.get('SAVE_END') == '1'
    save_file = os.path.join(SCRATCH, 'runs', name, 'mc0-saves', 'Career.sav')

    def end_save(st):
        # (SAVE_END=1) the run ended from the pause (END RUN, YES), its summary, GAME OVER's SAVE GAME (native saves:
        # the game's own save, a file); then a CROSS or UP now and then until the file is there (the save screen's
        # questions, No first), and a while after it
        screen = run.mem(0x2A2D70, 1)[0]          # g_gameScreen (bmxmp.SCREEN)
        if os.environ.get('SAVE_PADLOG') == '1' and screen in (SCREEN_GAME_OVER, SCREEN_OBJECTIVES):
            # (diagnosis) the game's pad state each tick: g_pads[0] (0x2F2B30, rider.h PadInput) held +0x10, changed +0x14
            held, changed = struct.unpack('<II', run.mem(0x2F2B40, 8))
            log(st, 'pad: screen %d held %04x changed %04x' % (screen, held, changed))
        if screen != start.get('save_screen'):
            start['save_screen'] = screen
            start['save_screen_at'] = st.script
            log(st, 'save: screen %d mode %d riding %d' % (screen, st.mode, st.riding))
            if gs:
                log(st, 'shot', run.shot('save-s%d-%d' % (st.script, screen)))
        if 'save_prog' not in start:
            # END RUN (the pause's last line but one: UP wraps to it), EXIT? YES
            start['save_prog'] = presses('START', 60, 'UP', 'CROSS', 150, 'DOWN', 'CROSS', 200)
            log(st, 'end run, save game')
        if start['save_prog']:
            return start['save_prog'].pop(0)
        if start.pop('save_cross', False):
            if gs:
                log(st, 'shot', run.shot('save-cursor-s%d' % st.script))
            start['save_prog'] = presses('CROSS', 120)
            return start['save_prog'].pop(0)
        if os.path.exists(save_file):
            start.setdefault('saved_at', st.script)
            if st.script == start['saved_at']:
                log(st, 'saved: %s %d bytes' % (save_file, os.path.getsize(save_file)))
                meter.s.count('saves')
            return None if st.script - start['saved_at'] > 600 else core.NOTHING
        k = start['save_k'] = start.get('save_k', 0) + 1
        if k > 4000:
            log(st, 'NOT saved')
            return None
        if screen == SCREEN_GAME_OVER and not start.get('save_chosen'):
            # GAME OVER: ANOTHER RUN, SAVE GAME, VIEW RECORDS, EXIT TO MAIN MENU (the cursor on the first)
            start['save_chosen'] = True
            start['save_prog'] = presses(int(os.environ.get('SAVE_WAIT', '200')), 'DOWN', int(os.environ.get('SAVE_GAP', '30')))
            start['save_cross'] = True
            return start['save_prog'].pop(0)
        if screen == SCREEN_GAME_OVER:
            return core.NOTHING
        # the run's summaries (CONTINUE) before GAME OVER, the save's own questions after it (No first: UP, CROSS); a
        # screen is answered once it has been up a while, so a press meant for the last one never lands on the next
        if st.script - start['save_screen_at'] < 90:
            return core.NOTHING
        if start.get('save_chosen'):
            return core.pad('UP' if (k // 120) % 2 else 'CROSS') if k % 120 < 6 else core.NOTHING
        return core.pad('CROSS') if k % 120 < 3 else core.NOTHING

    def policy(st):
        meter.tick(st)
        keep(st)
        watch_goals(st)
        if 'save_prog' in start:
            return end_save(st)
        if program:
            # a menu interlude runs (the pause menu's screens): its presses, whatever the state
            return program.pop(0)
        if start.get('t') is not None and pending_programs and st.riding and \
                st.script - start.get('after', start['t']) >= pending_programs[0][0]:
            t0, prog = pending_programs.pop(0)
            start['after'] = st.script + len(prog)
            log(st, 'interlude at %d (%d frames)' % (st.script - start['t'], len(prog)))
            played_programs.append(st.script - start['t'])
            program.extend(prog)
            return program.pop(0)
        if start.get('t') is not None:
            # A goal's cutscene or dialog (the giver's): the level's frame stops or the mode leaves 2.
            state = (st.mode, st.riding)
            if state != seen.get('state'):
                log(st, 'mode %d riding %d' % state)
                seen['state'] = state
            if shot_every and (st.script - start['t']) % shot_every == 0:
                log(st, 'shot', run.shot('p%d' % (st.script - start['t'])))
            if not st.riding:
                return core.pad('CROSS') if st.script % 60 < 6 else core.NOTHING
        if not st.riding:
            return core.NOTHING
        if st.pos[0] == 0.0 and st.pos[2] == 0.0:         # the level's overview: CONTINUE
            return core.pad('CROSS') if st.script % 40 < 6 else core.NOTHING
        start.setdefault('t', st.script)
        t = st.script - start['t']
        if t > ticks and save_end:
            return end_save(st)
        if t > ticks:
            return None
        if t < DIALOG_TICKS:                               # the level's CHALLENGES list: CONTINUE
            return core.pad('CROSS') if 40 <= t < 46 else core.NOTHING
        return pl(st)

    with core.Run(cfg) as run:
        pl.run = run
        core.play(run, read, policy, max_ticks=ticks + menu.t + 40000, recorder=rec, trace=trace)
    trace.close()
    text = menu.text() + ',' + rec.text()
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(text)
    stats = meter.s.json()
    meta = dict(name=name, level=level, rider=rider, seed=seed, ticks=ticks, end=run.script_time, end_tick=run.tick, start=start.get('t'),
                env=env)
    stats['areas'] = len(pl.seen)
    stats['goals'] = goals.get('best', {})
    stats['interludes'] = played_programs
    stats['flatlands'] = pl.flatlands
    core.save_json(os.path.join(d, 'stats.json'), stats)
    core.save_json(os.path.join(d, 'meta.json'), meta)
    print(name, meter.s.row(dm2.COLUMNS), 'areas', len(pl.seen), 'start', start.get('t'))


def test(name, program, ticks=1200, level=0, every=60):
    """From the level's start, a fixed program of (ticks, Pad): how the rider answers, printed every so often."""
    import dm2  # noqa: F401  (the dm2 tools' folder, bmxroutes in it, on the path)
    menu = menu_script(level)
    start = {}

    def policy(st):
        if not st.riding:
            return core.NOTHING
        if st.pos[0] == 0.0 and st.pos[2] == 0.0:
            return core.pad('CROSS') if st.script % 40 < 6 else core.NOTHING
        start.setdefault('t', st.script)
        t = st.script - start['t']
        if t > ticks:
            return None
        if t < DIALOG_TICKS:
            return core.pad('CROSS') if 40 <= t < 46 else core.NOTHING
        t -= DIALOG_TICKS
        if t % every == 0 or st.action != start.get('action'):
            start['action'] = st.action
            print(t, 'ph', st.physics, 'act', st.actions, 'pos %.1f %.1f %.1f' % st.pos, 'v %.3f' % st.speed, 'vel %.2f %.2f %.2f' % st.vel, 'fwd %.2f %.2f' % (st.fwd[0], st.fwd[2]), flush=True)
        at = 0
        for n, p in program:
            at += n
            if t < at:
                return p
        return core.NOTHING

    with core.Run(run_config(name, menu.text(), max_secs=170)) as run:
        core.play(run, read, policy, max_ticks=ticks + menu.t + 30000)


# The levels in the level select's order: folder, the cheat that opens it (the Bronx is open from the start).
LEVELS = (('CHL_MULY', None), ('CHL_DAM', 'THATDAMLEVEL'), ('CMP_RMPG', 'IOWARULES'), ('CHL_SHEP', 'BAABAA'), ('CHL_SYRA', 'BOYBANDSSUCK'),
          ('CMP_ROOT', 'UNDERGROUND'), ('CHL_MNCO', 'SHOWMETHEMONEY'), ('CHL_NASA', 'SHOWMETHEMONKEY'))


def main():
    a = sys.argv[1:]
    if a and a[0] == 'pedal':
        test('pedal', [(120, core.NOTHING), (900, core.pad('UP'))], ticks=1100, every=90)
    if a and a[0] == 'matrix':
        prog = [(60, core.NOTHING)]
        tries = [core.pad(b) for b in ('CROSS', 'SQUARE', 'CIRCLE', 'TRIANGLE', 'R1', 'R2', 'L1', 'L2')] + [core.pad('UP', lx=0.3), core.pad('UP', 'R1')]
        for p in tries:
            prog += [(120, p), (60, core.NOTHING)]
        test('matrix', prog, ticks=1900, every=60)
    if a and a[0] == 'first':
        test('first', [(60, core.NOTHING), (40, core.pad('UP', lx=0.0)), (60, core.NOTHING), (40, core.pad(lx=0.5)), (200, core.NOTHING), (200, core.pad('UP')),
                       (120, core.NOTHING), (200, core.pad('DOWN')), (120, core.NOTHING), (120, core.pad('CROSS')), (200, core.NOTHING)], ticks=1000, every=40)
    if a and a[0] == 'second':
        test('second', [(60, core.NOTHING), (80, core.pad('UP', lx=0.5)), (200, core.NOTHING), (120, core.pad('DOWN')), (200, core.NOTHING), (200, core.pad('UP')),
                        (120, core.NOTHING), (120, core.pad('CROSS')), (200, core.NOTHING)], ticks=1000, every=40)
    if a and a[0] == 'stick':
        test('stick', [(120, core.NOTHING), (900, core.pad(ly=-1.0))], ticks=1100, every=90)
    if a and a[0] == 'aim':
        # (routes2) standing still: the second stick held (the still rider's aim), then each fire candidate
        N = core.NOTHING
        prog = [(200, N), (90, core.pad(rx=1.0)), (60, core.pad(ry=-1.0))]
        for b in ('R1', 'R2', 'L1', 'L2', 'CROSS', 'SQUARE', 'CIRCLE', 'TRIANGLE'):
            prog += [(10, core.pad(b, rx=0.5)), (40, core.pad(rx=0.5))]
        prog += [(120, N)]
        test('aim', prog, ticks=900, level=int(a[1]) if len(a) > 1 else 0, every=30)
    if a and a[0] == 'flat':
        # (routes2) the flatland's way in: speed, the manual's two stick taps (up, then down), trick A (CIRCLE) with
        # the stick tapped sideways; variants by argument
        k = int(a[1]) if len(a) > 1 else 0
        N, U, D, L, R = core.NOTHING, core.pad(ly=-1.0), core.pad(ly=1.0), core.pad(lx=-1.0), core.pad(lx=1.0)
        prog = [(60, N), (150, core.pad('UP'))]
        taps = [(3, U), (3, N), (3, D), (3, N)] if k % 2 == 0 else [(3, D), (3, N), (3, D), (40, core.pad(ly=0.5))]
        prog += taps + [(20, core.pad(ly=0.3))]
        b = os.environ.get('FLAT_BUTTON', 'CIRCLE')
        trick = [(3, L), (2, N), (3, R), (2, N), (6, core.pad(b)), (30, N)]
        if k >= 2:
            # the d-pad's taps with the button held through them
            trick = [(4, core.pad(b, 'LEFT')), (3, core.pad(b)), (4, core.pad(b, 'RIGHT')), (3, core.pad(b)), (30, N)]
        if k == 4:
            # held through: the d-pad tapped on and on with the button down
            S = core.pad(b)
            trick = [(4, core.pad(b, 'LEFT')), (3, S), (4, core.pad(b, 'RIGHT')), (3, S)] * 6 + [(30, N)]
        if k in (6, 7, 8):
            # other tap pairs: up/down, down/down, left/left, held through
            d1, d2 = {6: ('UP', 'DOWN'), 7: ('DOWN', 'DOWN'), 8: ('LEFT', 'LEFT')}[k]
            S = core.pad(b)
            trick = [(4, core.pad(b, d1)), (3, S), (4, core.pad(b, d2)), (60, S), (20, N)]
        if k == 5:
            # stood still: the coast to a stop, then the manual's taps and the trick from standing
            prog = [(60, N), (150, core.pad('UP')), (240, N)] + taps + [(20, core.pad(ly=0.3))]
        prog += trick * 4 + [(120, N)]
        test('flat%d' % k, prog, ticks=900, level=int(a[2]) if len(a) > 2 else 0, every=30)
    if a and a[0] == 'replay':
        replay(a[1], spec=a[2] if len(a) > 2 else None)
    if a and a[0] == 'route':          # route NAME ROUTE: a bmxroutes route alone, what it saved
        route(a[1], a[2])
    if a and a[0] == 'states':         # states NAME SPEC FIRST LAST [WORDS]
        states(a[1], a[2], int(a[3]), int(a[4]), a[5] if len(a) > 5 else None)
    if a and a[0] == 'reach':
        reach(int(a[1]), int(a[2]))
    if a and a[0] == 'look':
        look(a[1], int(a[2]), int(a[3]))
    if a and a[0] == 'play':
        play_session(a[1], int(a[2]), seed=int(a[3]) if len(a) > 3 else 1, ticks=int(a[4]) if len(a) > 4 else 5400,
                     rider=int(a[5]) if len(a) > 5 else 0, gs=os.environ.get('GS') == '1',
                     max_secs=int(os.environ.get('MAX_SECS', '420')))


if __name__ == '__main__':
    main()
