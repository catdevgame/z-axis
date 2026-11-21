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
