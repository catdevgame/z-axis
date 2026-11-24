#!/usr/bin/env python3
"""How to ride DM2: the closed loop on the rider's state. dm2.py runs it; dm2map.py gives the level."""
import math
import os
import random

import core
from core import pad, wrap, clamp

GROUND, AIR, BAIL, GRIND_APPROACH, GRIND, STALL_APPROACH, STALL, LIP_APPROACH, LIP, NOSE_WHEELIE, MANUAL = range(11)
GRAVITY = 0.0055          # units a frame a frame (g_gravity)
AREA = 48                 # the grid of a level's areas for coverage, units
TRICK_A, TRICK_B, GRIND_BUTTON, HOP = 'CIRCLE', 'SQUARE', 'TRIANGLE', 'CROSS'
# Knobs for trick choice by DM2's scoring (score.cpp: a line is its entries' sum times the line factor, up to x3 at
# eight tricks; a held trick adds its points a unit; a spin multiplies the entry): VARIETY=1 avoids a direction and
# button already used in the line, HOLD_MAX the frames a trick is held at most, SPIN_P how often a long air spins.
VARIETY = os.environ.get('VARIETY') == '1'
HOLD_MAX = int(os.environ.get('HOLD_MAX', '44'))
SPIN_P = float(os.environ.get('SPIN_P', '0.4'))
# SAFE_FLIPS: the frames of air below which no flip is asked (down: backflip, downright: tailwhip; d4's bails were
# mostly flips and tailwhips landed unfinished, kinds 5, 7 and 8)
SAFE_FLIPS = int(os.environ.get('SAFE_FLIPS', '0'))
FLIP_DIRECTIONS = ('down', 'downright')
MIN_AIR = int(os.environ.get('MIN_AIR', '0'))     # the frames of air a trick is asked in (0: 34 in a line, else 30)
DIRS = {'left': (-1, 0), 'right': (1, 0), 'up': (0, -1), 'down': (0, 1), 'upleft': (-1, -1), 'upright': (1, -1),
        'downleft': (-1, 1), 'downright': (1, 1)}


def heading(st):
    return math.atan2(st.fwd[2], st.fwd[0])


def steer(st, target, gain=2.2):
    """LX towards a world point: positive LX turns the heading angle down."""
    want = math.atan2(target[2] - st.pos[2], target[0] - st.pos[0])
    err = wrap(want - heading(st))
    return clamp(-err * gain, -1.0, 1.0), err


class Log:
    def __init__(self, path):
        self.f = open(path, 'w')

    def __call__(self, st, *text):
        self.f.write('%d f%d (%.1f %.1f %.1f) v%.2f ph%d %s\n' % (st.script, st.frame, st.pos[0], st.pos[1], st.pos[2], st.speed, st.physics,
                                                                  ' '.join(str(t) for t in text)))
        self.f.flush()


class Air:
    """What to do in the air: tricks timed to what is left of it."""

    def __init__(self, player):
        self.p = player
        self.reset()

    def reset(self):
        self.program = []
        self.frames = 0
        self.left = 0
        self.lip_try = 0
        self.wall_try = 0
        self.tapped = False
        self.spin = 0
        self.spun = 0.0

    def landing_in(self, st):
        """Frames to the floor along the rider's path, by the level's floors."""
        lv = self.p.level
        x, y, z = st.pos
        vx, vy, vz = st.vel
        for n in range(1, 400):
            x += vx
            z += vz
            vy += GRAVITY
            y += vy
            if n % 4 == 0 and vy > 0:
                f = lv.nodes.get(core_cell(x, z))
                if f and any(abs(h[0] - y) < 1.6 and h[0] >= y - 0.4 for h in f):
                    return n
        return 400

    def plan(self, st, left):
        """A trick for this air: taps, then the button held, let go in time. By the frames there are."""
        rng = self.p.rng
        kinds = []
        if left >= (MIN_AIR or (34 if self.p.lining() else 30)):
            d = rng.choice(('left', 'right', 'up', 'down', 'upleft', 'upright', 'downleft', 'downright'))
            button = rng.choice((TRICK_A, TRICK_B))
            if SAFE_FLIPS and left < SAFE_FLIPS and d in FLIP_DIRECTIONS:
                # a flip not finished at the landing is a bail (the line lost): a held trick in a short air
                d = rng.choice(('left', 'right', 'up', 'upleft', 'upright', 'downleft'))
            if VARIETY:
                # DM2's variety factor (scoring: x0.75, 0.5, 0.25 for a trick and modifier done again in the line):
                # a direction and button not used yet in this line, while there is one.
                fresh = [(dd, b) for dd in DIRS for b in (TRICK_A, TRICK_B) if (dd, b) not in self.p.line_used
                         and not (SAFE_FLIPS and left < SAFE_FLIPS and dd in FLIP_DIRECTIONS)]
                if fresh:
                    d, button = rng.choice(fresh)
                self.p.line_used.add((d, button))
            tail = 22 if self.p.lining() else 16
            hold = clamp(left - tail - 8, 8, HOLD_MAX)
            self.program = []
            for t in range(3):
                self.program.append(pad(lx=DIRS[d][0] * 0.95, ly=DIRS[d][1] * 0.95))
            self.program.append(core.NOTHING)
            self.program += [pad(button)] * int(hold)
            self.p.log(st, 'trick', d, button, 'hold', hold, 'air left', left)
        if left >= 60 and rng.random() < SPIN_P:
            self.spin = rng.choice((180, 360, 360, 540 if left > 90 else 360))
            self.spin_button = rng.choice(('L1', 'R1'))
            self.last_heading = heading(st)
            self.spun = 0.0
            self.p.log(st, 'spin', self.spin, self.spin_button)

    def wall_near(self, st):
        """A wall alongside the rider, by the level's upright faces: the normal, if it runs along it."""
        cx, cz = int(st.pos[0] // 4), int(st.pos[2] // 4)
        size = math.hypot(st.vel[0], st.vel[2]) or 1.0
        for i in (-1, 0, 1):
            for k in (-1, 0, 1):
                for c, n in self.p.walls.get((cx + i, cz + k), ()):
                    d = abs((st.pos[0] - c[0]) * n[0] + (st.pos[2] - c[2]) * n[2])
                    if d < 2.5 and abs(c[1] - st.pos[1]) < 3 and abs(st.vel[0] * n[0] + st.vel[2] * n[2]) / size < 0.5:
                        return n
        return None

    def step(self, st):
        self.frames += 1
        # Near the top of a ramp's air: a lip trick or wall ride (tricks 0x3F .. 0x45): the stick, the crouch button pressed.
        if self.p.lips and not self.program and self.frames > 6 and abs(st.vel[1]) < 0.05 and self.lip_try == 0 and self.p.near_ramp_top(st):
            self.lip_try = 1
            self.p.log(st, 'lip ask')
            self.program = [pad(ly=-0.95), pad(HOP, ly=-0.95), pad(HOP, ly=-0.95), pad(HOP, ly=-0.95)]
        if self.p.wall_rides and self.frames > 4 and not self.program and self.wall_try == 0 and st.speed > 0.15 and self.wall_near(st):
            self.wall_try = 1
            d = self.p.rng.choice((0.95, -0.95))
            self.p.log(st, 'manual ask')
            self.program = [pad(ly=d)] * 3 + [core.NOTHING] * 3 + [pad(ly=d)] * 3 + [core.NOTHING] + [pad(ly=d)] * 40
        if self.frames == 3:
            self.left = self.landing_in(st)
            if st.vel[1] < 0:                  # a ramp's air: back at this height after 2 v / g
                self.left = min(self.left, int(-2 * st.vel[1] / GRAVITY) + 6)
            elif self.left >= 400:
                self.left = 0                  # coming down onto what the map does not know: no trick
            if self.p.tricks:
                self.plan(st, self.left)
        elif st.vel[1] > 0 and self.frames % 4 == 0:
            known = self.landing_in(st)
            if known < 400:
                self.left = known
        if self.left:
            self.left -= 1
        out = self.program.pop(0) if self.program else core.NOTHING
        if self.spin:
            h = heading(st)
            self.spun += abs(wrap(h - self.last_heading))
            self.last_heading = h
            if math.degrees(self.spun) >= self.spin - 70:      # it carries on a little
                self.spin = 0
            else:
                out = core.Pad(out.buttons | {self.spin_button}, out.lx, out.ly, out.rx, out.ry)
        # The stick pulled back as the wheels come down: the landing goes into a manual, the line goes on.
        if self.p.lining() and st.vel[1] > 0 and 0 < self.left < 13 and not self.tapped and not self.program:
            # Two taps down, the second held: on the ground that is the manual (riding state's two taps), so the wheels
            # come down into it and the line goes on.
            self.tapped = True
            self.program = [pad(ly=0.98)] * 3 + [core.NOTHING] * 3 + [pad(ly=0.98)] * 16
        elif self.p.link_lines and st.vel[1] > 0 and 0 < self.left < 6 and st.line_tricks and not self.program and out.ly is None:
            out = core.Pad(out.buttons, out.lx, 0.98, out.rx, out.ry)
        return out


def closest(p, e):
    """The point of segment e nearest p."""
    a, b = e
    d = [b[i] - a[i] for i in range(3)]
    n = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
    t = 0.0 if n < 1e-9 else clamp(sum((p[i] - a[i]) * d[i] for i in range(3)) / n, 0.0, 1.0)
    return [a[i] + d[i] * t for i in range(3)]


def core_cell(x, z):
    import nav
    return nav.cell_of(x, z)


class Task:
    """One thing to do on the ground. step(st) -> a Pad, or None when done."""
    name = 'task'
    limit = 900        # frames

    def __init__(self, player):
        self.p = player
        self.frames = 0

    def step(self, st):
        raise NotImplementedError


class Follow:
    """Pure pursuit along a path of world points."""

    def __init__(self, path):
        self.path = path
        self.i = 0

    def target(self, pos, look=9.0):
        p = self.path
        while self.i < len(p) - 1 and core.dist2(xz(p[self.i]), xz(pos)) < 16.0:
            self.i += 1
        j = self.i
        while j < len(p) - 1 and core.dist2(xz(p[j]), xz(pos)) < look * look:
            j += 1
        return p[j]

    def off(self, pos):
        return min(core.dist2(xz(q), xz(pos)) for q in self.path[max(0, self.i - 3):self.i + 6])

    def done(self, pos, near=3.0):
        return core.dist2(xz(self.path[-1]), xz(pos)) < near * near


def xz(p):
    return p[0], p[2]


class GoTo(Task):
    name = 'goto'

    def __init__(self, player, goal, near=4.0):
        super().__init__(player)
        self.goal, self.near = goal, near
        self.follow = None
        self.tried = 0

    def plan(self, st):
        path = self.p.level.path(st.pos, self.goal)
        self.tried += 1
        self.follow = Follow(path) if path else None

    def step(self, st):
        self.frames += 1
        if self.frames > self.limit:
            return None
        if self.follow is None or (self.frames % 60 == 0 and self.follow.off(st.pos) > 100):
            if self.tried > 4:
                return None
            self.plan(st)
            if self.follow is None:
                return None
        if self.follow.done(st.pos, self.near):
            return None
        lx, err = steer(st, self.follow.target(st.pos))
        return pad('UP', lx=lx)


class Speed(Task):
    """Pedal along a line through `through`, past it, at full speed: at a ramp or a rail."""
    name = 'line'

    def __init__(self, player, through, frames=240):
        super().__init__(player)
        self.through, self.limit = through, frames

    def step(self, st):
        self.frames += 1
        if self.frames > self.limit:
            return None
        lx, err = steer(st, self.through, gain=1.6)
        return pad('UP', lx=lx)


class Ramp(Task):
    """Ride to the foot of a ramp, then up it at speed. The air does the rest."""
    name = 'ramp'

    def __init__(self, player, face):
        super().__init__(player)
        self.face = face
        c, n = face['centre'], face['normal']
        size = math.hypot(n[0], n[2]) or 1.0
        self.out = (n[0] / size, n[2] / size)
        self.foot = (c[0] + self.out[0] * 16, c[1], c[2] + self.out[1] * 16)
        self.sub = GoTo(player, self.foot, near=5.0)
        self.line = None

    def step(self, st):
        self.frames += 1
        if self.sub:
            r = self.sub.step(st)
            if r is not None:
                return r
            self.sub = None
            if core.dist2(xz(st.pos), xz(self.foot)) > 100:
                return None        # could not get there
            self.line = Speed(self.p, (self.face['centre'][0] - self.out[0] * 6, 0, self.face['centre'][2] - self.out[1] * 6), 150)
            self.p.log(st, 'ramp run', self.face['kind'], self.face['strength'])
        r = self.line.step(st)
        if r is None or st.physics != GROUND:
            return None
        return r


class Combo(Task):
    """On the flat: a manual, a hop out of it, a trick in the air, a manual on landing, on and on: one line."""
    name = 'combo'
    limit = 600

    def __init__(self, player, target):
        super().__init__(player)
        self.target = target
        self.phase = 0
        self.t = 0
        self.links = 0

    def step(self, st):
        self.frames += 1
        if self.frames > self.limit or self.links > 5:
            return None
        lx, err = steer(st, self.target, gain=1.2)
        if abs(err) > 1.2 and self.phase == 0 and self.frames > 30:
            return None
        self.t += 1
        if self.phase == 0:              # speed
            if st.speed > 0.2 or self.t > 90:
                self.phase, self.t = 1, 0
                self.p.log(st, 'manual')
            return pad('UP', lx=lx)
        if self.phase == 1:              # the manual: the stick pulled back, the shoulder keeps the line straight
            if self.t > 44:
                self.phase, self.t = 2, 0
            return pad(ly=0.98, lx=lx * 0.3)
        if self.phase == 2:              # crouch out of it
            if self.t > 16:
                self.phase, self.t = 3, 0
                self.links += 1
            return pad(HOP, ly=0.98, lx=lx * 0.3)
        self.phase, self.t = 1, 0        # landed: the manual again
        return pad(ly=0.98, lx=lx * 0.3)


class Explore(GoTo):
    name = 'explore'


class Line(Task):
    """A long way over open ground as one line: speed, a hop, a trick, the manual on landing, a hop out of it, on and on.
    A line's score is its points times its tricks, so its length is what counts."""
    name = 'long line'
    limit = 900

    def __init__(self, player, goal):
        super().__init__(player)
        self.goal = goal
        self.follow = None
        self.charge = 0

    def next_goal(self, st):
        self.goal = self.p.far_cell(st) or self.goal
        path = self.p.level.path(st.pos, self.goal)
        self.follow = Follow(path) if path else None

    def step(self, st):
        self.frames += 1
        if self.frames > self.limit:
            return None
        if self.follow is None or self.follow.done(st.pos, 10.0):
            self.next_goal(st)
            if self.follow is None:
                return None
        lx, err = steer(st, self.follow.target(st.pos, 14.0))
        if self.charge:
            self.charge += 1
            if self.charge > self.p.crouch:
                self.charge = 0
                return pad('UP', lx=lx)
            return pad('UP', HOP, lx=lx)
        if st.speed > 0.17 and abs(err) < 0.5:
            self.charge = 1
            self.p.log(st, 'line start')
        return pad('UP', lx=lx)


class Rail(Task):
    """To a rail's run-up, along its line, a hop as high as it needs: the reflex (Player.rail_ahead) asks for the grind."""
    name = 'rail'

    def __init__(self, player, entry):
        super().__init__(player)
        self.entry = entry
        self.pts, self.t = entry['pts'], entry['tangent']
        self.sub = GoTo(player, entry['start'], near=6.0)
        self.hop = 0

    def step(self, st):
        self.frames += 1
        if self.sub:
            r = self.sub.step(st)
            if r is not None:
                return r
            self.sub = None
            if core.dist2(xz(st.pos), xz(self.entry['start'])) > 196:
                return None
            self.p.log(st, 'rail run', 'len %.0f' % self.entry['length'], 'rise %.1f' % self.entry['rise'])
            self.frames = 0
        a = self.pts[0]
        ahead = (a[0] - st.pos[0]) * self.t[0] + (a[2] - st.pos[2]) * self.t[1]
        s = max(0.0, -ahead) + 8.0
        lx, err = steer(st, (a[0] + self.t[0] * s, 0, a[2] + self.t[1] * s), gain=1.8)
        rise = st.pos[1] - a[1]
        if self.frames > 200 or ahead < -30 or rise > 3.0:
            self.p.log(st, 'rail run ends: frames %d ahead %.1f rise %.1f hop %d' % (self.frames, ahead, rise, self.hop))
            return None
        # the hop's height: v^2 / 2g with v = 0.09 .. about 0.17 by frames held (of 50)
        frames = int(clamp(50 * (math.sqrt(2 * GRAVITY * max(rise + 0.5, 0.1)) - 0.09) / 0.08, 4, 50))
        lead = st.speed * (frames + 10)
        if st.physics == GROUND and self.hop == 0 and ahead < lead:
            self.hop = 1
            self.hold = frames
            self.p.log(st, 'hop', frames, 'ahead %.1f rise %.1f' % (ahead, rise))
        if self.hop and self.hop < self.hold:
            self.hop += 1
            return pad('UP', HOP, lx=lx)
        return pad('UP', lx=lx)


class Player:
    """The closed loop: what the rider's physics state asks for now, then the task at hand, then the next task."""

    def __init__(self, level, seed, log):
        self.level, self.rng, self.log = level, random.Random(seed), log
        self.air = Air(self)
        self.tricks = True
        self.last_physics = 0
        self.task = None
        self.lean_gain = float(os.environ.get('LEAN', '2'))
        self.explorer = False
        self.targets, self.ridden = set(), set()
        self.given_up, self.tries = {}, {}
        self.bail_at, self.bail_repeats, self.bail_noted = None, 0, False
        self.line_share = float(os.environ.get('LINE_SHARE', '0.55'))
        self.crouch = int(os.environ.get('CROUCH', '24'))
        self.line_most = int(os.environ.get('LINE_MOST', '99'))     # tricks in a line before it is banked (no link)
        self.line_used = set()          # (direction, button) done in this line (VARIETY)
        self.rail_only = os.environ.get('RAILS') == '1'
        self.visited = set()
        self.stuck = 0
        self.grind_frames = 0
        self.bails = 0
        self.edges = {}
        for r in level.rails:
            for u, v in zip(r['points'], r['points'][1:]):
                n = max(1, int(math.dist(u, v) / 6))
                for i in range(n + 1):
                    x = u[0] + (v[0] - u[0]) * i / n
                    z = u[2] + (v[2] - u[2]) * i / n
                    self.edges.setdefault((int(x // 6), int(z // 6)), set()).add((u, v))
        self.walls = {}
        for c, n in level.wall_faces:
            self.walls.setdefault((int(c[0] // 4), int(c[2] // 4)), []).append((c, (n[0], n[1], n[2])))
        self.tops = {}
        for f in level.ramps:
            self.tops.setdefault((int(f['centre'][0] // 4), int(f['centre'][2] // 4)), []).append(f['top'])
        self.lips = os.environ.get('LIPS', '1') == '1'
        self.link_lines = True
        self.link = self.link_from = 0
        self.stuck_at, self.stuck_count, self.escape, self.bail_now = None, 0, 0, 0
        self.wheel_frames = 0
        self.wheel_hold = 60
        self.wall_rides = os.environ.get('WALLS', '1') == '1'
        self.seen = set()
        self.floor_cells = None
        self.turn_round = 0
        self.slow = 0
        self.rails = []
        for r in level.rails:
            if r['length'] < 7:
                continue
            for pts in (r['points'], r['points'][::-1]):
                a = self.rail_entry(pts)
                if a:
                    a['tag'] = r['name']
                    self.rails.append(a)
        self.ramps = []
        for f in level.ramps:
            if f['kind'] in (1, 3) and -0.9 < f['normal'][1] < -0.2:
                n = math.hypot(f['normal'][0], f['normal'][2]) or 1.0
                foot = (f['centre'][0] + f['normal'][0] / n * 16, f['centre'][1], f['centre'][2] + f['normal'][2] / n * 16)
                node = level.nearest(foot, radius=3)
                if node:
                    self.ramps.append(dict(f, part=level.part.get(node)))
        log.f.write('rail ways %d ramps %d\n' % (len(self.rails), len(self.ramps)))

    def near_ramp_top(self, st):
        """Over a ramp's top edge, by its faces, at about that height (y grows down)."""
        cx, cz = int(st.pos[0] // 4), int(st.pos[2] // 4)
        for i in (-1, 0, 1):
            for k in (-1, 0, 1):
                for top in self.tops.get((cx + i, cz + k), ()):
                    if -1.5 < st.pos[1] - top < 1.5:
                        return True
        return False

    def rail_entry(self, pts):
        """A way onto a rail from one end: a run-up in the main part, the rail a hop above it."""
        a, b = pts[0], pts[1]
        d = math.hypot(b[0] - a[0], b[2] - a[2])
        if d < 0.5:
            return None
        t = ((b[0] - a[0]) / d, (b[2] - a[2]) / d)
        for back in (14, 20, 26):
            start = (a[0] - t[0] * back, a[1], a[2] - t[1] * back)
            node = self.level.nearest(start, radius=2)
            if not node or not self.reachable(start):
                continue
            floor = self.level.world(node)
            rise = floor[1] - a[1]
            if -0.4 <= rise <= 2.1 and math.hypot(floor[0] - start[0], floor[2] - start[2]) < 5:
                return dict(pts=pts, length=math.dist(a, pts[-1]), rise=rise, start=floor, tangent=t, part=self.level.part.get(node))
        return None

    def reachable(self, p):
        node = self.level.nearest(p)
        if not node:
            return False
        part = self.level.part.get(node)
        return part is not None and self.level.sizes[part] > 2000

    def rail_ahead(self, st):
        """What the game's grind search would find now (grindApproachEnter): an edge within 3.5, ahead, near the path."""
        cx, cz = int(st.pos[0] // 6), int(st.pos[2] // 6)
        found = set()
        for i in (-1, 0, 1):
            for k in (-1, 0, 1):
                found |= self.edges.get((cx + i, cz + k), set())
        near = []
        for e in found:
            q = closest(st.pos, e)
            if sum((a - b) ** 2 for a, b in zip(q, st.pos)) <= 3.5 * 3.5:
                near.append(e)
        if not near:
            return None
        p, v = list(st.pos), list(st.vel)
        reach = 0.1 if st.physics == AIR else 0.2
        for step in range(30):
            for e in near:
                q = closest(p, e)
                ahead = sum((q[i] - st.pos[i]) * st.vel[i] for i in range(3))
                if ahead >= 0 and sum((q[i] - p[i]) ** 2 for i in range(3)) <= reach * reach:
                    return e
            p = [p[i] + v[i] for i in range(3)]
            v[1] += GRAVITY
            reach += 0.03
        return None

    def __call__(self, st):
        if st.physics != self.last_physics:
            self.log(st, 'physics', self.last_physics, '->', st.physics, 'trick %x' % st.trick, 'line', st.line_tricks, st.line_score, 'score', st.run_score,
                     'vy %.3f' % st.vel[1], 'bail %d' % st.bail_kind if st.physics == BAIL else '')
            if st.physics == GROUND and self.last_physics == AIR and (st.line_tricks or self.lining()) and self.link_lines and st.line_tricks < self.line_most and self.rng.random() < (1.0 if self.lining() else 0.7):
                self.link = self.link_from = self.rng.randint(36, 80)
            if st.physics in (NOSE_WHEELIE, MANUAL):
                self.wheel_frames = 0
                self.wheel_hold = self.rng.randint(20, 34) if self.lining() else self.rng.randint(50, 170)
            if st.physics == AIR:
                self.air.reset()
                self.link = 0
            if st.physics in (GRIND, STALL, LIP):
                self.grind_frames = 0
                self.task = None
            self.last_physics = st.physics
            self.log(st, 'balance %.2f up %.2f %.2f %.2f' % (st.balance, *st.up))
        if st.line_tricks == 0 and st.physics == GROUND:
            self.line_used = set()
        if st.physics == BAIL:
            if self.task is not None or not self.bail_noted:
                # The same bail in the same place again (a respawn facing a wall): turn round before anything else.
                again = self.bail_at and core.dist2(xz(self.bail_at), xz(st.pos)) < 25.0
                self.bail_repeats = self.bail_repeats + 1 if again else 0
                self.bail_at = st.pos
                self.bail_noted = True
                if self.bail_repeats >= 1:
                    self.turn_round = 120
                    self.away = wrap(heading(st) + math.pi + self.rng.uniform(-0.8, 0.8))
                    self.given_up[(int(st.pos[0] // AREA), int(st.pos[2] // AREA))] = 1
            self.task = None
            return core.NOTHING
        self.bail_noted = False
        if st.physics in (GRIND, STALL, LIP):
            return self.on_rail(st)
        if st.physics in (NOSE_WHEELIE, MANUAL):
            return self.on_one_wheel(st)
        if st.physics in (GRIND_APPROACH, STALL_APPROACH, LIP_APPROACH):
            return core.NOTHING
        # (draft2) a task that must not bail (a timed goal's run, AIR_QUIET=1, or the task's own quiet_air): no
        # trick, spin, lip or grind ask in the air
        if st.physics == AIR and (os.environ.get('AIR_QUIET') == '1' or getattr(self.task, 'quiet_air', False)):
            self.cover(st)
            return core.NOTHING
        answer = self.reflex(st)
        if answer:
            return answer
        if st.physics == AIR:
            self.cover(st)
            if self.task and isinstance(self.task, Rail):
                return pad(GRIND_BUTTON)
            if self.task and isinstance(self.task, Combo):
                self.task.phase = 3
            return self.air.step(st)
        return self.ride(st)

    def reflex(self, st):
        # In the air the button held asks for a grind every frame (airState), if the taken air has begun.
        if st.physics == AIR and st.speed > 0.08 and not self.air.program:
            if self.rail_ahead(st):
                return pad(GRIND_BUTTON)
        return None

    def lining(self):
        return isinstance(self.task, Line)

    def on_one_wheel(self, st):
        """The manual and the nose wheelie (states 10 and 9; the HUD's balance bar): the balance walks with the stick's y; at +-1 one side is down to the ground, the other a bail.
        Keep it level, then crouch and hop: the line goes on."""
        self.wheel_frames += 1
        f = self.wheel_frames
        ly = clamp(-st.balance * 3.0, -1.0, 1.0)
        lx = None
        if self.lining() and self.task.follow:
            lx = steer(st, self.task.follow.target(st.pos, 14.0))[0] * 0.5
        if f > self.wheel_hold:
            return pad(HOP, ly=ly, lx=lx) if f < self.wheel_hold + self.crouch else pad(ly=ly, lx=lx)
        return pad(ly=ly, lx=lx)

    def on_rail(self, st):
        self.grind_frames += 1
        f = self.grind_frames
        if st.physics in (STALL, LIP):       # held, then let go
            return pad(HOP, ly=-0.95) if f < 34 else core.NOTHING
        lean = clamp(self.lean_gain * st.balance, -1, 1)
        if st.physics == GRIND and st.speed < 0.08 and f > 30:
            return pad(HOP) if f % 16 < 10 else core.NOTHING      # slowing down: hop off
        if f % 70 == 40:
            self.rail_trick = self.rng.choice(('left', 'right', 'up', 'down'))
        if f % 70 in (40, 41, 42) and self.tricks:
            d = DIRS[self.rail_trick]
            return pad(lx=d[0] * 0.95, ly=d[1] * 0.95)
        if f % 70 in range(44, 56) and self.tricks:
            return pad(TRICK_B, lx=lean)
        return pad(lx=lean)

    def cover(self, st):
        self.seen.add((int(st.pos[0] // AREA), int(st.pos[2] // AREA)))

    def far_cell(self, st):
        """The nearest floor of the main part in an area not ridden yet."""
        if self.floor_cells is None:
            import nav
            main = max(range(len(self.level.sizes)), key=lambda n: self.level.sizes[n])
            part = self.level.main_part(st.pos)
            use = part if part is not None and self.level.sizes[part] > 1000 else main
            cells = {}
            for (key, layer), n in self.level.part.items():
                if n == use:
                    w = nav.centre(*key)
                    cells.setdefault((int(w[0] // AREA), int(w[1] // AREA)), (key, layer))
            self.floor_cells = cells
        left = [c for k, c in self.floor_cells.items() if k not in self.seen and k not in self.given_up]
        if not left:
            return None
        here = (st.pos[0], st.pos[2])
        best = min(left, key=lambda c: core.dist2(xz(self.level.world(c)), here) + self.rng.random() * 400)
        k = next(a for a, c in self.floor_cells.items() if c == best)
        self.given_up[k] = self.given_up.get(k, 0)           # one try, then the others come first
        self.tries[k] = self.tries.get(k, 0) + 1
        if self.tries[k] < 2:
            del self.given_up[k]
        return self.level.world(best)

    def next_task(self, st):
        rng = self.rng
        here = xz(st.pos)
        # A long rail is worth a ride: points by the frame on it, and the line's factor.
        part = self.level.main_part(st.pos)
        near_rails = sorted((r for r in self.rails if r['part'] == part), key=lambda r: (core.dist2(here, xz(r['start'])) + rng.random() * 900 + (4e4 if id(r) in self.visited else 0)) / max(1.0, min(r['length'], 60.0)))[:3]
        ramps = [f for f in self.ramps if f['part'] == part]
        near_ramps = sorted(rng.sample(ramps, min(120, len(ramps))), key=lambda f: core.dist2(here, xz(f['centre'])) + rng.random() * 400)[:3]
        if os.environ.get('LINES') == '1':
            return Line(self, st.pos)
        wanted = [r for r in self.rails if r['part'] == part and r['tag'] in self.targets and r['tag'] not in self.ridden]
        if wanted and rng.random() < 0.6:
            r = min(wanted, key=lambda w: core.dist2(here, xz(w['start'])) + rng.random() * 400)
            self.ridden.add(r['tag'])
            self.log(st, 'target rail %x' % r['tag'])
            return Rail(self, r)
        faces = [f for f in self.ramps if f['part'] == part and f.get('tag') in self.targets and f.get('tag') not in self.ridden]
        if faces and rng.random() < 0.6:
            f = min(faces, key=lambda w: core.dist2(here, xz(w['centre'])) + rng.random() * 400)
            self.ridden.add(f['tag'])
            self.log(st, 'target ramp %x' % f['tag'])
            return Ramp(self, f)
        if self.explorer:                      # Free Ride: every area of the level, a line now and then on the way
            far = self.far_cell(st)
            if far and rng.random() < 0.9:
                self.log(st, 'explore to %.0f %.0f, %d areas seen' % (far[0], far[2], len(self.seen)))
                return Explore(self, far, near=10.0)
        k = 0.0 if self.rail_only else rng.random()
        if k < self.line_share:
            return Line(self, st.pos)
        k = rng.random()
        if k < 0.42 and near_rails:
            r = near_rails[0]
            self.visited.add(id(r))
            return Rail(self, r)
        if k < 0.72 and near_ramps:
            return Ramp(self, near_ramps[0])
        if k < 0.86:
            a = heading(st)
            return Combo(self, (st.pos[0] + math.cos(a) * 60, 0, st.pos[2] + math.sin(a) * 60))
        far = self.far_cell(st)
        if far:
            self.log(st, 'explore to %.0f %.0f, %d areas seen' % (far[0], far[2], len(self.seen)))
            return Explore(self, far, near=8.0)
        return Ramp(self, near_ramps[0]) if near_ramps else None

    def ride(self, st):
        self.cover(st)
        if self.link:
            # A manual out of the landing, a crouch, a hop: the next air's trick joins the line.
            self.link -= 1
            lx = None
            if self.task and getattr(self.task, 'follow', None):
                lx = steer(st, self.task.follow.target(st.pos))[0] * 0.3
            if self.link == 0:
                return core.NOTHING
            f = self.link_from - self.link
            if f < 3:
                return pad(ly=0.98, lx=lx)
            if f < 6:
                return pad(lx=lx)
            return pad(HOP, ly=0.98, lx=lx) if self.link < 16 else pad(ly=0.98, lx=lx)
        if st.speed < 0.04:
            self.slow += 1
        else:
            self.slow = 0
        if self.bail_now:
            self.bail_now -= 1
            return pad('L1', 'R1', 'L2', 'R2')
        if self.escape:
            self.escape -= 1
            return pad(HOP, lx=0.95) if self.escape > 12 else pad('UP', lx=0.95)
        if self.turn_round:
            # Standing, the stick turns the bike on the spot (the riding state's still turn); then away.
            self.turn_round -= 1
            if self.turn_round > 40:
                err = wrap(self.away - heading(st))
                if abs(err) > 0.35:
                    return pad(lx=clamp(-err * 2, -1, 1))
                self.turn_round = 40
            return pad('UP')
        if self.slow > 40:
            self.slow = 0
            self.task = None
            again = self.stuck_at and core.dist2(xz(self.stuck_at), xz(st.pos)) < 4.0
            self.stuck_count = self.stuck_count + 1 if again else 1
            self.stuck_at = st.pos
            self.log(st, 'stuck', self.stuck_count)
            if self.stuck_count >= 5:      # wedged: what a player does, the bail button (all four shoulders), and start over
                self.stuck_count = 0
                self.bail_now = 8
            elif self.stuck_count >= 2:    # a hop, turning
                self.escape = 40
            else:
                self.turn_round = 120
                self.away = wrap(heading(st) + math.pi + self.rng.uniform(-0.8, 0.8))
        if self.task is None:
            self.task = self.next_task(st)
            if self.task:
                self.log(st, 'task', self.task.name)
        if self.task is None:
            return pad('UP')
        r = self.task.step(st)
        if r is None:
            self.task = None
            return pad('UP')
        return r
