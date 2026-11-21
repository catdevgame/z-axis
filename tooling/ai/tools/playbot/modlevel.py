#!/usr/bin/env python3
"""Aggressive Inline: a custom level served from a Mods folder, ridden in free skate by ai.py's player.

    modlevel.py build FOLDER [SLOT]            the level's navigation map from the folder's own files
                                               (FOLDER/ASSETS/LEVELS/SLOT/SLOT.*, a Mods folder's layout; SLOT TUT):
                                               parkmap.py's map, the one a park gets, kept as $PLAYAI/level/<SLOT>.nav
                                               like a disc level's
    modlevel.py ride NAME FOLDER TICKS [SEED] [--entry K] [--shots N] [--tour]
                                               boot, FREE SKATE, the first skater, level-select entry K (the Estate's,
                                               7: level index 7, whose files the mod replaces), then TICKS pad reads of
                                               riding: rails, lines of hops and manuals, the quarter pipes; the game's
                                               own numbers printed at the end (distance, grinds and rail metres, airs,
                                               score, bails). --shots N: the GS on, N pictures along the way in the
                                               run's shots/ folder (the first when placed, the rest on rails and in airs);
                                               --tour: the map's features in turn (aipark.tour) instead of the lines;
                                               the features reached printed either way (parkmap.Reached)

The run: ZX_MODS_DIR is FOLDER, ZX_ESTATE=1 with ZX_ESTATE_DIR (the app's Estate folder, AI_DATA/dist/.estate
unless set) so the free skate level select has an entry for level 7; mods are read before the Estate's provider, so
the level is the folder's. Everything else is ai.py's (its clock, card, scratch, Player and behaviours).
"""
import collections
import json
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..', '..'))
import zpaths  # noqa: E402
import ai  # noqa: E402
import core  # noqa: E402
import nav  # noqa: E402
import parkmap  # noqa: E402

ESTATE_DIR = os.environ.get('ZX_ESTATE_DIR', os.path.join(zpaths.AI_DATA, 'dist', '.estate'))


def build(folder, slot='TUT'):
    """The level's map from the folder's own files (parkmap.folder_map: the collision and grind edges ai_level reads,
    the same map a park gets), kept where nav.Level and ai.Plan look for a disc level's (.nav) and as a .parkmap."""
    out = parkmap.folder_map(folder, slot)
    path = os.path.join(nav.SCRATCH, 'level', slot + '.nav')
    parkmap.save(out, path)
    parkmap.save(out, os.path.join(nav.SCRATCH, 'level', slot + '.parkmap'))
    part, sizes = nav.components(out['nodes'])
    rails = out['rails']
    print('%s from %s: %d cells, %d parts (largest %d), %d rails (%s), %d points, %d walls; features %s' % (
        slot, folder, len(out['nodes']), len(sizes), max(sizes or [0]), len(rails),
        ', '.join('%s %.0f m kind %d' % (r['name'], r['length'], r['kind']) for r in rails), len(out['points']), len(out['walls']),
        dict(collections.Counter(f['kind'] for f in out['features']))), flush=True)
    return out


def free_skate(bot, k, skater=0):
    """From the main menu (the cursor on CAREER): FREE SKATE, skater `skater`, level select entry k, placed."""
    p0 = bot.s.panel
    yield from ai.tap(bot, 'RIGHT', gap=30)
    yield from ai.tap(bot, 'CROSS', gap=30)
    p1 = yield from ai.new_panel(bot, p0)
    yield from ai.set_cursor(bot, skater)
    yield from ai.tap(bot, 'CROSS', gap=30)
    p2 = yield from ai.new_panel(bot, p1)
    if bot.s.cursor == ai.NAME_ENTRY_END:
        yield from ai.tap(bot, 'CROSS', gap=30)
        yield from ai.new_panel(bot, p2)
    yield from ai.set_cursor(bot, k)
    print('free skate, skater', skater, 'level select entry', bot.s.cursor, flush=True)
    yield from ai.tap(bot, 'CROSS', gap=30)
    yield from ai.wait_placed(bot)


def ride(bot, plan, ticks, shots=None):
    """Free skate: rails, lines and launches in turn until `ticks`."""
    yield from ai.until(bot, lambda s: s.speed > 0.2, 900, core.pad('UP'))
    end = bot.s.tick + ticks
    n = 0
    while bot.s.tick < end:
        yield from ai.on_the_ground(bot)
        n += 1
        if n % 3 == 1:
            ok = yield from ai.rail_line(bot, plan)
        elif n % 3 == 2:
            ok = yield from ai.launch_line(bot, plan)
        else:
            ok = yield from ai.long_line(bot, plan)
        if shots and shots.due(bot, n):
            shots.take(bot)
        yield core.NOTHING
        if not ok:
            bot.tally('fail')
            yield from ai.on_the_ground(bot)
            if bot.s.speed < 3.0:
                yield from ai.unstick(bot)


class Shots:
    """Pictures at a few points of the ride: when placed, then every few lines."""

    def __init__(self, run, name, most):
        self.run, self.name, self.most, self.taken = run, name, most, 0

    def due(self, bot, n):
        return self.taken < self.most and n % 2 == 0

    def take(self, bot):
        self.taken += 1
        path = self.run.shot('%s-%d' % (self.name, self.taken))
        print('shot', self.taken, 'tick', bot.s.tick, 'at', [round(v, 1) for v in bot.s.pos], path, flush=True)


class Numbers:
    """The game's own numbers over the ride, from each read: distance along the ground, grinds and their metres and
    time, airs and their time, bails, score."""

    def __init__(self):
        self.last = None
        self.distance = 0.0
        self.grinds = self.airs = self.bails = 0
        self.grind_metres = self.grind_time = self.air_time = 0.0
        self.longest_grind = 0.0
        self.grind_start = None
        self.was_down = False
        self.score = 0
        self.ticks = 0
        self.tricks = set()

    def __call__(self, s, p):
        if not s.placed:
            return
        self.ticks += 1
        self.score = max(self.score, s.score)
        if self.last is not None:
            step = math.dist(s.pos, self.last.pos)
            if step < 1.0:              # a vsync's travel is under 0.3 m at the game's top speed: a respawn is no ride
                self.distance += step
                if s.grinding:
                    self.grind_metres += step
                    self.grind_time += 1 / 60.0
                    self.grind_start = (self.grind_start or 0.0) + step
            if s.grinding and not self.last.grinding:
                self.grinds += 1
                self.grind_start = 0.0
            if not s.grinding and self.last.grinding and self.grind_start is not None:
                self.longest_grind = max(self.longest_grind, self.grind_start)
                self.grind_start = None
            if s.air and not self.last.air:
                self.airs += 1
            if s.air:
                self.air_time += 1 / 60.0
            if s.down and not self.last.down:
                self.bails += 1
        self.last = s

    def text(self):
        return ('%d ticks placed: %.0f m ridden, %d grinds (%.1f m on rails, %.1f s, longest %.1f m), %d airs (%.1f s), %d bails, score %d'
                % (self.ticks, self.distance, self.grinds, self.grind_metres, self.grind_time, self.longest_grind, self.airs, self.air_time, self.bails, self.score))


def ride_session(name, folder, ticks, seed=1, entry=7, shots=0, slot='TUT', tour=False):
    m = build(folder, slot)
    plan = ai.Plan(slot)
    reached = parkmap.Reached(parkmap.Plan(m, run_up=14.0, clear=1.2))
    env = {'ZX_MODS_DIR': os.path.abspath(folder), 'ZX_ESTATE': '1', 'ZX_ESTATE_DIR': ESTATE_DIR}
    if shots:
        env['ZX_HEADLESS'] = '1'
    numbers = Numbers()
    pictures = []

    def behaviour(bot):
        yield from ai.boot(bot)
        yield from free_skate(bot, entry)
        print('placed tick', bot.s.tick, 'level', bot.s.level, 'at', [round(v, 1) for v in bot.s.pos], 'facing', [round(v, 2) for v in bot.s.forward], flush=True)
        taker = Shots(run, name, shots) if shots else None
        if taker:
            yield from ai.wait(bot, 60)
            taker.take(bot)
        if tour:                # the park players' tour of the map's features (aipark.tour)
            import aipark
            plan.lv = reached.plan
            yield from aipark.tour(bot, plan, ticks, reached)
        else:
            yield from ride(bot, plan, ticks, taker)

    rec = core.Recorder()
    for sub in ('measure', 'sessions', 'runs'):
        os.makedirs(os.path.join(ai.SCRATCH, sub), exist_ok=True)
    log = ai.Log(os.path.join(ai.SCRATCH, 'measure', name + '.samples.gz'))
    with core.Run(ai.config(name, env, secs=max(300, ticks // 40), gs=bool(shots))) as run:
        bot = ai.Player(run, behaviour, seed)

        def on_tick(s, p):
            log(s, p)
            numbers(s, p)
            if s.placed:
                reached.tick(s.pos, 'grind' if s.grinding else 'wall' if s.st == ai.WALL_RIDE else 'air' if s.air else None)

        try:
            core.play(run, ai.read, bot, ticks + 12000, on_tick=on_tick, recorder=rec)
        finally:
            log.close()
        if shots:
            pictures = sorted(os.path.join(run.dir, 'shots', f) for f in os.listdir(os.path.join(run.dir, 'shots')))
        print('tick', run.tick, 'level', bot.s.level, 'at', [round(v, 1) for v in bot.s.pos], flush=True)
        print('notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:16], flush=True)
    print(numbers.text(), flush=True)
    print('features', json.dumps(reached.summary()), flush=True)
    if pictures:
        print('pictures:', *pictures, flush=True)
    with open(os.path.join(ai.SCRATCH, 'sessions', name + '.script'), 'w') as f:
        f.write(rec.text())
    with open(os.path.join(ai.SCRATCH, 'sessions', name + '.env'), 'w') as f:
        json.dump(env, f)
    return numbers


def main(argv):
    if argv and argv[0] == 'build':
        build(argv[1], argv[2] if len(argv) > 2 else 'TUT')
        return
    if argv and argv[0] == 'ride':
        flags = {a: argv[i + 1] for i, a in enumerate(argv) if a.startswith('--')}
        words = [a for i, a in enumerate(argv[1:], 1) if not a.startswith('--') and not argv[i - 1].startswith('--')]
        ride_session(words[0], words[1], int(words[2]), seed=int(words[3]) if len(words) > 3 else 1,
                     entry=int(flags.get('--entry', 7)), shots=int(flags.get('--shots', 0)), tour='--tour' in argv)
        return
    raise SystemExit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
