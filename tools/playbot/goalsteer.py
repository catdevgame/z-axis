#!/usr/bin/env python3
"""Goal steering for the playbot (routes2): the game's own symbols by name, and tasks that take the rider to a goal's
place whether or not the level map's paths reach it.

    Symbols(game)            the build's symbol table (its generated sym headers: the readable code's names, at the
                             running game's addresses), so a player reads g_npcs, g_menuChoices ... by name
    Approach(player, where)  a task to a place that may move (where: a callable giving it each frame): the level's
                             paths while they lead there, then straight at it (raised ground: a hop when the rider is
                             held back), done within `near` (and within `rise` in height when given)

A player built on player_dm2 (dm2.py's, bmx.py's) runs these as its tasks; nothing here knows a game's addresses.
"""
import math
import os
import re

import core
import player_dm2 as pd


def build_dir():
    """The runner's build folder (RUNNER=.../runtime/zx-runner, or BUILD)."""
    if os.environ.get('BUILD'):
        return os.environ['BUILD']
    runner = os.environ.get('RUNNER', '')
    return os.path.dirname(os.path.dirname(runner)) if runner else ''


class Symbols:
    """{name: address} of a game's symbols, from the build's generated headers (runtime/symbols/GAME/sym): the
    game's own folder first, then the engine's (zge), ee and sce; another game's folder (sym/dm2 in BMX XXX's set:
    DM2 files built for BMX) only for a name none of those has."""

    LINE = re.compile(r'namespace sym(?:::\w+)? \{ inline constexpr uint32_t (\w+) = 0x([0-9A-Fa-f]+)u; \}')

    def __init__(self, game, build=None):
        root = os.path.join(build or build_dir(), 'runtime', 'symbols', game, 'sym')
        if not os.path.isdir(root):
            raise SystemExit('no symbol headers at %s (BUILD or RUNNER)' % root)
        order = [game, 'zge', 'ee', 'sce']
        rest = sorted(d for d in os.listdir(root) if d not in order)
        self.at = {}
        for top in order + rest:
            folder = os.path.join(root, top)
            if not os.path.isdir(folder):
                continue
            found = {}
            for base, _, files in os.walk(folder):
                for name in sorted(files):
                    if name.endswith('.h'):
                        for m in self.LINE.finditer(open(os.path.join(base, name), errors='replace').read()):
                            found.setdefault(m.group(1), int(m.group(2), 16))
            for k, v in found.items():
                self.at.setdefault(k, v)

    def __getitem__(self, name):
        return self.at[name]

    def get(self, name, default=None):
        return self.at.get(name, default)


def flat(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


def main_cell(level, pos):
    """The level map's cell of its largest part nearest pos (across), as a world point; None when pos is in it."""
    main = max(range(len(level.sizes)), key=lambda n: level.sizes[n])
    here = level.nearest(pos)
    if here is not None and level.part.get(here) == main:
        return None
    cells = getattr(level, '_main_cells', None)
    if cells is None:
        cells = level._main_cells = [level.world(n) for n, k in level.part.items() if k == main]
    return min(cells, key=lambda w: flat(w, pos)) if cells else None


class Approach(pd.Task):
    """To a place (where(st) -> a world point or None while it is not there to go to), by the level's paths while the
    rider is far and a path leads near it, then straight at it. A rider held back on the straight run (slow while
    pedalling: a kerb, a step up) hops; done within `near` across (and `rise` in height, y down, when given), or None
    at `limit` frames. `slow` caps the speed on the last stretch (a person or a rider to stop beside)."""
    name = 'approach'

    def __init__(self, player, where, near=3.0, rise=None, limit=1500, slow=None, label=''):
        super().__init__(player)
        self.where, self.near, self.rise, self.limit, self.slow = where, near, rise, limit, slow
        self.label = label
        self.sub = None
        self.sub_goal = None
        self.straight = 0
        self.held = 0
        self.hop = 0
        self.closest = 1e9
        self.reached = False

    def step(self, st):
        self.frames += 1
        goal = self.where(st)
        if goal is None or self.frames > self.limit:
            if self.frames > self.limit:
                self.p.log(st, 'approach %s: not reached, closest %.1f' % (self.label, self.closest))
            return None
        d = flat(st.pos, goal)
        self.closest = min(self.closest, d)
        if d < self.near and (self.rise is None or abs(st.pos[1] - goal[1]) < self.rise):
            self.reached = True
            self.p.log(st, 'approach %s: reached, %.1f across, %.1f in height' % (self.label, d, st.pos[1] - goal[1]))
            return None
        if d < self.near:
            # under or over it, not at it (another floor): given up after a while
            self.off_height = getattr(self, 'off_height', 0) + 1
            if self.off_height > 120:
                self.p.log(st, 'approach %s: %.1f across but %.1f in height: another floor' % (self.label, d, st.pos[1] - goal[1]))
                return None
        # off the map's main part (a pocket the map links to nothing): straight to its nearest cell of the main
        # part first, then by the paths
        if getattr(self, 'escape', None) is not None:
            if flat(st.pos, self.escape) < 1.5 or self.escape_frames > 300:
                self.p.log(st, 'approach %s: %s the main part' % (self.label, 'on' if flat(st.pos, self.escape) < 1.5 else 'not on'))
                self.escape = None
                self.sub = None
            else:
                self.escape_frames += 1
                lx, _ = pd.steer(st, self.escape)
                return core.pad('UP', lx=lx)
        # by the paths while far: a new path when the place has moved on
        if d > 14.0 and self.straight == 0:
            if self.sub is None or flat(self.sub_goal, goal) > 6.0:
                self.sub, self.sub_goal = pd.GoTo(self.p, goal, near=10.0), goal
                # (draft2) a long way across a level takes longer than GoTo's 900 frames (Swamp's west bank: 240
                # units round the water); gone straight at it after that, the rider rode into the water
                self.sub.limit = max(self.sub.limit, int(d / 0.12))
            r = self.sub.step(st)
            if r is not None:
                return r
            if self.sub.frames <= 1 and getattr(self, 'escapes', 0) < 2:
                main = main_cell(self.p.level, st.pos)
                if main is not None and flat(main, st.pos) < 30.0:
                    self.escapes = getattr(self, 'escapes', 0) + 1
                    self.escape, self.escape_frames = main, 0
                    self.p.log(st, 'approach %s: no path from here, to the main part at (%.1f %.1f %.1f)' % (self.label, *main))
                    lx, _ = pd.steer(st, main)
                    return core.pad('UP', lx=lx)
            self.straight = 1      # the paths end (or lead no nearer): straight on from here
            self.p.log(st, 'approach %s: paths end %.1f away, straight at it (%.1f %.1f %.1f)' % (self.label, d, *goal))
        self.straight += 1
        # (draft2) far off still after a while straight on (the paths given up after a bail or a detour: Swamp's
        # trees, run into at speed, cost the rider its health and the run): the paths again from here
        if d > 20.0 and self.straight % 240 == 0 and getattr(self, 'replans', 0) < 6:
            self.replans = getattr(self, 'replans', 0) + 1
            self.straight, self.sub = 0, None
            self.p.log(st, 'approach %s: %.1f off, the paths again' % (self.label, d))
            return core.pad('UP')
        # held back near it (a fence, a kerb, the thing's own body): round it, a quarter turn about it, then in again
        if getattr(self, 'round', 0):
            self.round -= 1
            a = math.atan2(st.pos[2] - goal[2], st.pos[0] - goal[0]) + self.round_side * 1.2
            side = (goal[0] + math.cos(a) * 4.0, goal[1], goal[2] + math.sin(a) * 4.0)
            lx, _ = pd.steer(st, side)
            return core.pad('UP', lx=lx)
        lx, err = pd.steer(st, goal)
        if self.hop:
            self.hop -= 1
            return core.pad('UP', pd.HOP, lx=lx) if self.hop > 6 else core.pad('UP', lx=lx)
        if st.speed < 0.05 and abs(err) < 0.6:
            self.held += 1
        else:
            self.held = 0
        if self.held > 30 and st.physics == pd.GROUND:
            self.held = 0
            self.blocked = getattr(self, 'blocked', 0) + 1
            if d < 8.0 and self.blocked % 2 == 0:
                self.round, self.round_side = 60, 1 if (self.blocked // 2) % 2 else -1
                self.p.log(st, 'approach %s: held %.1f off, round it' % (self.label, d))
                return core.pad('UP', lx=lx)
            self.hop = 22
            return core.pad('UP', pd.HOP, lx=lx)
        if self.slow is not None and d < 10.0 and st.speed > self.slow:
            return core.pad(lx=lx, ly=1.0)     # the stick pulled back: brake
        if self.slow is not None and d < 10.0 and st.speed > self.slow * 0.6:
            return core.pad(lx=lx)
        return core.pad('UP', lx=lx)


def floor_under(level, x, z, y, around=0):
    """The level map's floor (y down) nearest under height y at x, z, and its node: (height, node) or (None, None).
    around=N: the cells up to N away too (a point beside a post or a sign, whose own cell holds only its top)."""
    import nav
    ci, ck = nav.cell_of(x, z)
    best = None
    for di in range(-around, around + 1):
        for dk in range(-around, around + 1):
            key = (ci + di, ck + dk)
            for n, (h, _) in enumerate(level.nodes.get(key, ())):
                if h >= y - 0.3 and (best is None or (abs(di) + abs(dk), h) < best[0]):
                    best = ((abs(di) + abs(dk), h), h, (key, n))
    return (best[1], best[2]) if best else (None, None)


def main_floor_near(level, point, radius):
    """The level map's floor of its main part nearest point (3D) within radius across, as a world point, or None."""
    import nav
    main = max(range(len(level.sizes)), key=lambda n: level.sizes[n])
    ci, ck = nav.cell_of(point[0], point[2])
    r = int(radius / nav.CELL) + 1
    best = None
    for di in range(-r, r + 1):
        for dk in range(-r, r + 1):
            key = (ci + di, ck + dk)
            c = nav.centre(*key)
            if math.hypot(c[0] - point[0], c[1] - point[2]) > radius:
                continue
            for n, (h, _) in enumerate(level.nodes.get(key, ())):
                if level.part.get((key, n)) != main:
                    continue
                d = math.dist((c[0], h, c[1]), point)
                if best is None or d < best[0]:
                    best = (d, (c[0], h, c[1]))
    return best[1] if best else None


class Reach(pd.Task):
    """(draft2) Through a point that floats over the floors (a pickup a goal hangs in the air: BMX XXX's Scores coins
    1.2 to 7.5 over the ground), until taken() says the game took it. Each try picks a run-up: up a ramp face beside
    the point (its launch carries the rider up through it), else a straight line over the level map's main part at the
    point's floor; the rider is taken to the run-up's start by Approach, then pedals through the point at full speed,
    hopping where the hop's top meets it when the point is out of the riding body's reach. Every try logs how near the
    drawn body came (3D) and how high it went over the floor, so a miss says which way to change.

    point: the place (world, y down); drawn(st): the drawn body's place (what the game measures a pickup's radius
    from); reach: the radius the game takes it within (a guess until a take is seen); ramps: the level map's ramp faces
    (dict centre, normal, kind, top) or ()."""
    name = 'reach'
    quiet_air = True        # (player_dm2) no trick in its air: a bail costs a timed goal its time

    def __init__(self, player, point, taken, drawn, label, reach=2.5, ramps=(), tries=6, limit=3000, shot=None):
        super().__init__(player)
        self.point, self.taken, self.drawn, self.label, self.shot = point, taken, drawn, label, shot
        self.reach, self.tries, self.limit = reach, tries, limit
        self.level = player.level
        self.ramps = [r for r in ramps if flat(r['centre'], point) < 3.0 and r['normal'][1] < -0.2]
        self.try_n = 0
        self.ways = self.run_ups()
        self.phase, self.sub, self.way = 'plan', None, None
        self.best3d = 1e9
        self.result = None

    def run_ups(self, here=None):
        """[(start, aim, kind, hop_needed)] best first. `here`: the rider's place, so the ways that begin on its side
        come first (it is at speed already: a short run-up, no turn back).
          line    a straight run over the floor under the point (or beside it, within the reach), when the point is
                  within the riding body's reach over that floor; with a hop when it is not
          launch  (draft2: the Bronx's coin47, 4.8 over a box whose east end is a steep kicker the map leaves out) a
                  run over a lower floor, up a short stretch the map has no floor for (a face too steep to stand
                  on), onto the point's floor: the kicker's air carries the rider up through the point
          ramp    up a ramp face (kinds 1 to 3) beside the point, the quarter pipe's air
          direct  straight at it (Approach's paths, then its hops when held back), and a hop under it"""
        p = self.point
        ramp_ways = []
        for r in sorted(self.ramps, key=lambda r: flat(r['centre'], p)):
            n = r['normal']
            size = math.hypot(n[0], n[2]) or 1.0
            ox, oz = n[0] / size, n[2] / size          # out of the face: the run-up comes from there
            fh, _ = floor_under(self.level, p[0] + ox * 14.0, p[2] + oz * 14.0, r['top'], around=1)
            start = (p[0] + ox * 14.0, fh if fh is not None else r['centre'][1], p[2] + oz * 14.0)
            ramp_ways.append((start, (p[0] - ox * 6.0, r['top'], p[2] - oz * 6.0), 'ramp', False))
            break
        h, node = floor_under(self.level, p[0], p[2], p[1], around=1)
        main = max(range(len(self.level.sizes)), key=lambda n: self.level.sizes[n])
        lines, launches = [], []
        if node is not None and self.level.part.get(node) == main:
            for runup in (5, 8, 12):
                for k in range(16):
                    a = k * math.pi / 8
                    dx, dz = math.cos(a), math.sin(a)
                    # the side it comes from: towards the rider first
                    side = 0.0 if here is None else -((here[0] - p[0]) * dx + (here[2] - p[2]) * dz) / max(flat(here, p), 1e-3)
                    # through the point, or beside it within the reach (a post or a sign it hangs by may fill its
                    # own cells)
                    for off in (0.0, 0.7, -0.7, 1.4, -1.4, 2.0, -2.0):
                        if abs(off) > self.reach * 0.8:
                            continue
                        c = (p[0] - dz * off, p[2] + dx * off)
                        samples = []
                        for t in range(-runup, 4):
                            q = (c[0] + dx * t, c[1] + dz * t)
                            fh, fn = floor_under(self.level, q[0], q[1], h - 0.6)
                            samples.append(fh if fn is not None and self.level.part.get(fn) == main and abs(fh - h) <= 0.6 else None)
                        if all(x is not None for x in samples):
                            lines.append((-side, abs(off), runup, (c[0] - dx * runup, h, c[1] - dz * runup),
                                          (c[0] + dx * 5.0, h, c[1] + dz * 5.0)))
                            break
                    # a launch: lower floor, a short stretch with none (a steep kicker), then the point's floor
                    if runup == 12 and k not in [l[-1] for l in launches]:
                        below, gap, onto = [], 0, 0
                        for t in range(-16, 1):
                            q = (p[0] + dx * t, p[2] + dz * t)
                            fh, fn = floor_under(self.level, q[0], q[1], h - 0.6, around=0)
                            low, ln = floor_under(self.level, q[0], q[1], h + 0.8)
                            if fn is not None and abs(fh - h) <= 0.6 and self.level.part.get(fn) == main:
                                onto += 1
                            elif not onto and ln is not None and low - h > 1.0 and self.level.part.get(ln) == main:
                                below.append((t, low))
                                gap = 0
                            elif below and not onto:
                                gap += 1
                        if len(below) >= 4 and 1 <= gap <= 6 and onto >= 1:
                            t0, low = below[0]
                            launches.append((-side, (p[0] + dx * t0, low, p[2] + dz * t0), (p[0] + dx * 6.0, h, p[2] + dz * 6.0), k))
        line_ways = [(start, aim, 'line', True) for side, off, runup, start, aim in sorted(lines)[:8]]
        launch_ways = [(start, aim, 'launch', False) for side, start, aim, k in sorted(launches)[:4]]
        # (y down) how far the point is over the floor, against the riding body's centre (about 1 over it): within
        # the reach the line alone takes it; else a launch or a ramp's air first, the line's hop after
        if h is not None and (h - p[1]) - 1.0 < self.reach * 0.8:
            out = line_ways + ramp_ways + launch_ways
        else:
            out = launch_ways + ramp_ways + line_ways
        # last (or alone, with no clear run-up on the map)
        out.append((None, p, 'direct', False))
        return out

    def step(self, st):
        self.frames += 1
        if self.taken():
            self.p.log(st, 'reach %s: taken, try %d, the drawn body %.2f off' % (self.label, self.try_n, math.dist(self.drawn(st), self.point)))
            self.result = 'taken'
            return None
        if self.frames > self.limit:
            self.p.log(st, 'reach %s: not taken in %d frames, closest %.2f' % (self.label, self.limit, self.best3d))
            self.result = 'time'
            return None
        d3 = math.dist(self.drawn(st), self.point)
        self.best3d = min(self.best3d, d3)
        if self.phase == 'plan':
            if not self.ways or self.try_n >= self.tries:
                self.p.log(st, 'reach %s: no run-up left (%d tries), closest %.2f' % (self.label, self.try_n, self.best3d))
                self.result = 'no way'
                return None
            # the next way in turn (a line and its reverse are two ways)
            if self.try_n == 0:
                self.ways = self.run_ups(st.pos)
                # straight from here, when the map has a clear floor all the way and the point is in the body's
                # reach over it: no turn for a run-up (a timed goal's coins one after another)
                h, _ = floor_under(self.level, self.point[0], self.point[2], self.point[1], around=1)
                if h is not None and (h - self.point[1]) - 1.0 < self.reach * 0.8 and self.clear(st.pos, (self.point[0], h, self.point[2])):
                    d = flat(st.pos, self.point) or 1.0
                    aim = (self.point[0] + (self.point[0] - st.pos[0]) / d * 5.0, h, self.point[2] + (self.point[2] - st.pos[2]) / d * 5.0)
                    self.ways.insert(0, ('here', aim, 'line', True))
            self.way = self.ways[self.try_n % len(self.ways)]
            self.try_n += 1
            if self.way[0] == 'here':
                self.way = (st.pos,) + self.way[1:]
                self.phase, self.run, self.hopped, self.high, self.try_best = 'run', 0, False, 0.0, 1e9
                self.hop_frames = 0
                self.p.log(st, 'reach %s: try %d straight from here, %.1f off' % (self.label, self.try_n, flat(st.pos, self.point)))
            elif self.way[0] is None:
                h, node = floor_under(self.level, self.point[0], self.point[2], self.point[1], around=1)
                under = (self.point[0], self.point[1] if h is None else h, self.point[2])
                main = max(range(len(self.level.sizes)), key=lambda n: self.level.sizes[n])
                if node is None or self.level.part.get(node) != main:
                    # its floor is a pocket the map links to nothing (the Bronx's checkpoint on a one-cell ledge):
                    # the nearest floor of the main part beside it, within the reach
                    near = main_floor_near(self.level, self.point, self.reach + 1.0)
                    if near is not None:
                        under = near
                self.sub = Approach(self.p, lambda st2, u=under: u, near=0.8, limit=900, label='%s straight' % self.label)
                self.phase = 'direct'
                self.try_best = 1e9
                self.p.log(st, 'reach %s: try %d straight at it, %d ways' % (self.label, self.try_n, len(self.ways)))
            else:
                self.sub = Approach(self.p, lambda st2, w=self.way[0]: w, near=2.5, limit=1500, label='%s run-up' % self.label)
                self.phase = 'to start'
                self.p.log(st, 'reach %s: try %d by %s from (%.1f %.1f %.1f), %d ways' % (self.label, self.try_n, self.way[2], *self.way[0], len(self.ways)))
        if self.phase == 'direct':
            if d3 < self.try_best:
                self.try_best = d3
                if self.shot and d3 < 3.0 and not getattr(self, 'shot_taken', False):
                    self.shot_taken = True
                    self.p.log(st, 'shot', self.shot('%s-try%d' % (self.label, self.try_n)))
            if self.sub is not None:
                r = self.sub.step(st)
                if r is not None:
                    return r
                self.sub, self.hop_frames, self.after = None, 0, 0
                body = self.drawn(st)
                self.p.log(st, 'reach %s: try %d straight: at it, the drawn body %.2f off, %.2f under it' % (
                    self.label, self.try_n, d3, body[1] - self.point[1]))
            # under it, out of reach: a hop (HOP held while it rises), then the try ends on landing
            body = self.drawn(st)
            if body[1] - self.point[1] > self.reach * 0.6 and self.hop_frames < 14 and st.physics in (pd.GROUND, pd.AIR):
                self.hop_frames += 1
                lx, _ = pd.steer(st, self.point, gain=1.6)
                return core.pad(pd.HOP, lx=lx, ly=0.0) if flat(st.pos, self.point) < 1.0 else core.pad('UP', pd.HOP, lx=lx)
            self.after += 1
            if self.after < 60 and st.physics != pd.GROUND:
                return core.NOTHING
            self.p.log(st, 'reach %s: try %d straight: the drawn body %.2f off at closest' % (self.label, self.try_n, self.try_best))
            self.phase, self.shot_taken = 'plan', False
            return core.NOTHING
        if self.phase == 'to start':
            r = self.sub.step(st)
            if r is not None:
                return r
            if not self.sub.reached:
                self.phase = 'plan'
                return core.NOTHING
            self.phase, self.run, self.hopped, self.high, self.try_best = 'run', 0, False, 0.0, 1e9
            self.hop_frames = 0
        # the run: full speed at the point, past it to the aim; a hop when the point is over the body's reach
        self.run += 1
        self.try_best = min(self.try_best, d3)
        body = self.drawn(st)
        h, _ = floor_under(self.level, body[0], body[2], body[1])
        if h is not None:
            self.high = max(self.high, h - body[1])
        start, aim, kind, hop = self.way
        to_point = flat(st.pos, self.point)
        past = flat(st.pos, aim) < 2.0 or (self.run > 30 and flat(st.pos, start) > flat(self.point, start) + 4.0)
        if self.shot and self.run > 1 and d3 <= self.try_best and d3 < 4.0 and not getattr(self, 'shot_taken', False):
            self.shot_taken = True
            self.p.log(st, 'shot', self.shot('%s-try%d' % (self.label, self.try_n)))
        if past or self.run > 400 or st.physics == pd.BAIL:
            self.shot_taken = False
            self.p.log(st, 'reach %s: try %d (%s) missed: the drawn body %.2f off at closest, %.2f over the floor at '
                       'most, speed %.2f' % (self.label, self.try_n, kind, self.try_best, self.high, st.speed))
            self.phase = 'plan'
            return core.NOTHING
        lx, _ = pd.steer(st, aim if to_point < 1.0 else self.point, gain=1.6)
        over = self.point[1] - body[1]      # (y down: negative while the point is over the body)
        if hop and not self.hopped and over < -self.reach * 0.5 and st.physics == pd.GROUND and to_point < self.hop_at(st):
            self.hopped = True
            self.p.log(st, 'reach %s: hop %.1f before it, %.2f under it, speed %.2f' % (self.label, to_point, -over, st.speed))
        if self.hopped and st.physics in (pd.GROUND, pd.AIR) and self.hop_frames < 12:
            self.hop_frames += 1
            return core.pad('UP', pd.HOP, lx=lx)
        return core.pad('UP', lx=lx)

    def clear(self, a, b):
        """Whether the map has floor all the way from a to b (every unit along it, on the main part, no step over
        0.6 from the last)."""
        main = max(range(len(self.level.sizes)), key=lambda n: self.level.sizes[n])
        h, node = floor_under(self.level, a[0], a[2], a[1] - 0.6)
        if node is None:
            return False
        n = max(1, int(flat(a, b)))
        for i in range(1, n + 1):
            x, z = a[0] + (b[0] - a[0]) * i / n, a[2] + (b[2] - a[2]) * i / n
            fh, fn = floor_under(self.level, x, z, h - 0.6)
            if fn is None or self.level.part.get(fn) != main or abs(fh - h) > 0.6:
                return False
            h = fh
        return True

    def hop_at(self, st):
        """How far before the point the hop goes: its rise's frames at this speed (HOP_FRAMES, a guess the logs set)."""
        return max(1.5, st.speed * float(os.environ.get('HOP_FRAMES', '18')))
