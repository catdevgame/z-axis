#!/usr/bin/env python3
"""The two bikes on their editor parks (dm2park.py's, bmxpark.py's builds): ridden by the playbot, measured by
dm2.Meter (distance, rail, jumps, grinds, tricks, bails, score).

    bikepark.py dm2 NAME CARD TICKS [SEED]    DM2: FREERIDE, the first rider and bike, level select's 8 (the park),
                                              the card's first park (CARD: a card folder holding BMX2PARK.DAT)
    bikepark.py bmx NAME PARKS TICKS [SEED]   BMX XXX: PARK EDITOR > SESSION (bmxedit.py's ride_saved), the first park
                                              (PARKS: its Parks folder)

The circuit is aipark.py's ring in each park's own cells: the tower's roll-ins and kickers, the plaza's rail and box,
the lawn's long rail, the pool, the hangar's mini ramp, the yard's jump line, the bank back up. A bike pedals (UP),
steers (the left stick), hops (CROSS held: longer, higher), grinds (TRIANGLE held in the air over a rail), tricks in
a long air (a direction and CIRCLE, let go before the landing). SHOTS=1: pictures. Scratch: $PLAYDM2, $BMXCORPUS.
"""
import collections
import json
import math
import os
import random
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import dm2  # noqa: E402
import memuse  # noqa: E402
import parkmap  # noqa: E402
from core import pad, NOTHING, wrap, clamp  # noqa: E402

SHOTS = os.environ.get('SHOTS') == '1'
GROUND, AIR, BAIL, GRIND_APPROACH, GRIND, STALL_APPROACH, STALL, LIP_APPROACH, LIP, NOSE_WHEELIE, MANUAL = range(11)
HOP, GRIND_BUTTON, TRICK_A, TRICK_B = 'CROSS', 'TRIANGLE', 'CIRCLE', 'SQUARE'
GRAVITY = 0.0055          # (player_dm2.py) units a frame a frame; y grows down


class Park:
    """A park's cells in the world and its circuit (cell coordinates: (a, b) as the editor's cursor has them)."""

    def __init__(self, world, steps, zones):
        self.world, self.steps, self.zones = world, steps, zones


# DM2: (row, column) on Warehouse's 24 x 24 (edit_grid.cpp: x = column x 5 - 57.5, z = row x 5 - 57.5, y = -level)
DM2 = Park(
    world=lambda r, c: ((c - 11.5) * 5.0, 0.0, (r - 11.5) * 5.0),
    steps=[
        ('ramps', 'tower drop', (2.5, 9.5), (10, 9.5), 1),
        ('rail', 'plaza rail', (9, 13.4), (9, 11.6), 2.5),     # (the grind took 2.5 m to the line's side: rail_drill)
        ('rail', 'lawn rail', (8.4, 15), (3.6, 15), 0),
        ('ramps', 'pool', (4.5, 8), (4.5, 2.6), 2),
        ('rail', 'plaza box', (11.6, 11.4), (11.6, 8.6), 0),
        ('ramps', 'hangar mini ramp', (15.5, 4.5), (15.5, 6.6), 3),
        ('ramps', 'yard dirt line', (9, 19), (19, 19), 1),
        ('ramps', 'tower bank', (2.5, 15), (2.5, 10), 1),
    ],
    zones=(('tower', 2, 6, 7, 13), ('pools', 2, 11, 2, 6), ('hangar', 12, 22, 2, 9), ('yard', 12, 22, 11, 22),
           ('lawn', 2, 11, 13, 22), ('plaza', 7, 12, 6, 13)))
# BMX XXX: (x, z) on Mullaly's 30 x 20 (the selection box: x = (x - 14.5) x 5, z = -(z - 9.5) x 5), AI's park at +7, +2
BMX = Park(
    world=lambda x, z: ((x - 14.5) * 5.0, 0.0, -(z - 9.5) * 5.0),
    steps=[
        ('ramps', 'tower drop', (7.5, 9.5), (15, 9.5), 1),
        ('rail', 'plaza rail', (14, 11.6), (14, 13.4), 0),
        ('rail', 'lawn rail', (13.4, 15), (8.6, 15), 0),
        ('ramps', 'pool', (10.5, 7), (10.5, 1), 2),
        ('ramps', 'hangar mini ramp', (20.5, 4.5), (20.5, 6.6), 3),
        ('ramps', 'yard dirt line', (15, 17), (25, 17), 1),
        ('ramps', 'tower bank', (7.5, 15), (7.5, 10), 1),
    ],
    zones=(('tower', 7, 11, 7, 13), ('pools', 7, 16, 2, 6), ('hangar', 17, 23, 2, 9), ('yard', 17, 23, 11, 20),
           ('lawn', 7, 16, 13, 20), ('plaza', 12, 17, 6, 13)))


def heading(st):
    return math.atan2(st.fwd[2], st.fwd[0])


def steer_x(st, target, gain=2.2):
    want = math.atan2(target[2] - st.pos[2], target[0] - st.pos[0])
    return clamp(-wrap(want - heading(st)) * gain, -1.0, 1.0)


def flat(a, b):
    return math.hypot(a[0] - b[0], a[2] - b[2])


class Rider:
    def __init__(self, run, park, seed=1):
        self.run, self.park = run, park
        self.st = None
        self.rng = random.Random(seed)
        self.notes = {}
        self.air_frames = 0

    def tally(self, what):
        self.notes[what] = self.notes.get(what, 0) + 1

    def say(self, *what):
        print('%6d' % self.st.tick, *what, flush=True)


def wait(n, p=NOTHING):
    for _ in range(n):
        yield p


def tap(button, hold=6, gap=30):
    yield from wait(hold, pad(button))
    yield from wait(gap)


def in_air(r):
    """An air: a trick if it lasts (a direction and CIRCLE or SQUARE, held, let go about 14 frames before the
    landing), TRIANGLE held over a rail is the grind's ask; then the ground."""
    st = r.st
    up = -st.vel[1]                         # y grows down: rising is negative
    left = int(2 * up / GRAVITY) + 6 if up > 0 else 20
    trick = left > 40
    if trick:
        d = r.rng.choice(((0.0, -0.95), (0.95, 0.0), (-0.95, 0.0), (0.0, 0.95)))
        b = r.rng.choice((TRICK_A, TRICK_B))
        r.tally('air trick')
    n = 0
    while r.st.physics == AIR and n < 400:
        n += 1
        if trick and n < left - 14:
            yield pad(b, lx=d[0], ly=d[1]) if n < 6 else pad(b)
        else:
            yield pad(GRIND_BUTTON)


def on_rail(r):
    n = 0
    while r.st.physics in (GRIND, STALL, LIP, GRIND_APPROACH, STALL_APPROACH, LIP_APPROACH) and n < 900:
        n += 1
        st = r.st
        if st.physics in (GRIND_APPROACH, STALL_APPROACH, LIP_APPROACH):
            yield pad(GRIND_BUTTON)              # the button kept down while the bike settles on the rail
            continue
        if st.physics in (STALL, LIP):
            yield pad(HOP, ly=-0.95) if n < 34 else NOTHING
            continue
        lean = clamp(2.0 * st.balance, -1, 1)
        if n % 70 in range(44, 56):
            yield pad(TRICK_B, lx=lean)
        else:
            yield pad(lx=lean)
    r.tally('grind ridden')


def drive(r, target, near=3.0, limit=600):
    slow = 0
    for _ in range(limit):
        st = r.st
        if not st.riding:
            yield NOTHING
            continue
        if st.physics == BAIL:
            r.tally('bail')
            for _ in range(400):
                if r.st.physics != BAIL:
                    break
                yield NOTHING
            return False
        if st.physics == AIR:
            yield from in_air(r)
            continue
        if st.physics in (GRIND, STALL, LIP, GRIND_APPROACH, STALL_APPROACH, LIP_APPROACH):
            yield from on_rail(r)
            continue
        if st.physics in (NOSE_WHEELIE, MANUAL):
            yield pad('UP', ly=clamp(-2.0 * st.balance, -1, 1))
            continue
        if flat(st.pos, target) < near:
            return True
        slow = slow + 1 if st.speed < 0.03 else 0
        if slow > 60:
            r.tally('stuck')
            yield from wait(30, pad(HOP, lx=0.95))
            yield from wait(30, pad('UP', lx=0.95))
            slow = 0
            continue
        yield pad('UP', lx=steer_x(st, target))
    return False


def rail(r, a, b, what, shift=0.0):
    """A rail from cell point a to b: a run-up on its line (SHIFT metres to its right), the hop, the grind."""
    w = r.park.world
    on, end = w(*a), w(*b)
    d = (end[0] - on[0], end[2] - on[2])
    size = math.hypot(*d)
    d = (d[0] / size, d[1] / size)
    on = (on[0] - d[1] * shift, on[1], on[2] + d[0] * shift)
    return (yield from rail_world(r, on, (on[0] + d[0] * size, on[1], on[2] + d[1] * size), what))


HOP_RISE = 2.2           # the highest rail over its ground a hop reaches: v^2 / 2g with v at most 0.17 is 2.6


def rail_world(r, on, end, what, far=26.0, rise=0.8):
    """A rail met at `on` (the world), ridden towards `end`: to the run-up's start FAR metres back on its line, then
    along the line (steering at a point a little ahead on it, so the bike settles onto it however it came in), the
    hop once lined up and near enough for its height (v^2 / 2g), TRIANGLE held over the rail. Not lined up in time:
    round to the start again (twice at most)."""
    d = (end[0] - on[0], end[2] - on[2])
    size = math.hypot(*d)
    d = (d[0] / size, d[1] / size)
    way = math.atan2(d[1], d[0])
    frames = int(clamp(50 * (math.sqrt(2 * GRAVITY * max(rise + 0.4, 0.1)) - 0.09) / 0.08, 4, 50))
    got = False
    for attempt in range(3):
        ok = yield from drive(r, (on[0] - d[0] * far, 0, on[2] - d[1] * far), near=4.0)
        if not ok:
            return False
        hopped, lined_up = 0, False
        for n in range(int(far * 20) + 240):
            st = r.st
            if st.physics in (GRIND, GRIND_APPROACH, STALL, STALL_APPROACH):
                got = True
                break
            if st.physics == BAIL:
                break
            rx, rz = st.pos[0] - on[0], st.pos[2] - on[2]
            along, across = rx * d[0] + rz * d[1], rz * d[0] - rx * d[1]
            if along > size or (hopped and st.physics == GROUND and hopped > 40):
                break
            look = clamp(st.speed * 50, 4.0, 9.0)
            lx = steer_x(st, (on[0] + d[0] * (along + look), 0, on[2] + d[1] * (along + look)), gain=1.8)
            # where the hop would come down: the grind search reaches 3.5 units round the bike, so the rail is taken
            # from the side too; the hop goes when that place is on the rail's first two thirds
            lead = frames + 10
            px, pz = st.pos[0] + st.vel[0] * lead - on[0], st.pos[2] + st.vel[2] * lead - on[2]
            p_along, p_across = px * d[0] + pz * d[1], pz * d[0] - px * d[1]
            if st.physics == GROUND and not hopped and 0.0 <= p_along <= size * 0.67:
                if abs(p_across) > 1.5 or abs(wrap(way - heading(st))) > 0.6:
                    r.say('  %s: not lined up at along %.1f across %.1f heading off %.2f speed %.2f' % (
                        what, along, across, wrap(way - heading(st)), st.speed))
                    break               # too far off to come down on it
                hopped = 1
            if st.physics == GROUND and not hopped and p_along > size * 0.67:
                break
            if SHOTS and hopped in (1, 12, 24):
                print('shot', r.run.shot('%s-hop%d-%d' % (what.split()[0], hopped, r.st.tick)), 'phys', st.physics, flush=True)
            if hopped and hopped < frames:
                hopped += 1
                yield pad('UP', HOP, lx=lx)
            elif hopped:
                hopped += 1
                yield pad(GRIND_BUTTON, lx=lx)
            else:
                yield pad('UP', lx=lx)
        if got or hopped:
            break
        r.tally('%s: run-up again' % what)
    st = r.st
    rx, rz = st.pos[0] - on[0], st.pos[2] - on[2]
    r.say('  %s: %s at along %.1f across %.1f phys %d' % (what, 'ON' if got else 'off', rx * d[0] + rz * d[1], rz * d[0] - rx * d[1], st.physics))
    r.tally('%s: %s' % (what, 'grind' if got else 'missed'))
    if got:
        yield from on_rail(r)
    return got


def ramps(r, a, b, what, passes=1):
    w = r.park.world
    for k in range(passes):
        ok = yield from drive(r, w(*a), near=3.0)
        ok = ok and (yield from drive(r, w(*b), near=2.0, limit=400))
        r.tally('%s %s' % (what, 'through' if ok else 'short'))
        a, b = b, a


def rail_drill(r, ticks):
    """(RIDE=rails) The first rail of the circuit again and again, its line shifted across: where the grind takes."""
    kind, what, a, b, n = [step for step in r.park.steps if step[0] == 'rail'][0]
    end = r.st.tick + ticks
    k = 0
    while r.st.tick < end:
        shift = [float(v) for v in os.environ.get("SHIFTS", "-1.5,-0.75,0,0.75,1.5").split(",")][k % len(os.environ.get("SHIFTS", "-1.5,-0.75,0,0.75,1.5").split(","))]
        k += 1
        got = yield from rail(r, a, b, '%s shifted %+.2f' % (what, shift), shift)
        yield from drive(r, r.park.world(*a), near=4.0)


def circuit(r, ticks):
    end = r.st.tick + ticks
    lap = 0
    while r.st.tick < end and r.park.steps:
        lap += 1
        r.say('lap %d at %s score %.0f' % (lap, [round(v, 1) for v in r.st.pos], r.st.run_score))
        for kind, what, a, b, n in r.park.steps:
            if r.st.tick >= end:
                break
            t0 = r.st.tick
            if kind == 'rail':
                yield from rail(r, a, b, what, shift=n)
            else:
                yield from ramps(r, a, b, what, passes=n)
            r.say('  %s done in %d at %s score %.0f' % (what, r.st.tick - t0, [round(v, 1) for v in r.st.pos], r.st.run_score))


def wander(r, end, places):
    """Nothing left to tour and no cell circuit (a level's map, not a park's): from one feature's place to another,
    picked at random, until END (the circuit would loop without a step and never let the game run)."""
    rng = random.Random(r.st.tick)
    if not places:
        while r.st.tick < end:
            yield pad('UP')
        return
    r.say('wandering between %d places' % len(places))
    while r.st.tick < end:
        yield from goto(r, rng.choice(places), near=4.0)
        yield pad('UP')


def goto(r, target, near=3.0):
    """To a point over the map's cells (parkmap.Plan.path): every third cell a waypoint, the last near `near`."""
    path = r.plan.path(r.st.pos, target) if r.plan else None
    if path:
        for p in path[2:-3:3]:
            ok = yield from drive(r, p, near=3.5, limit=300)
            if not ok:
                return False
    return (yield from drive(r, target, near=near, limit=600))


def tour(r, ticks):
    """(the default) The park's features from its own map (parkmap.py): the nearest not yet ridden, along a path to its
    run-up's start; rails ground, ramps ridden up into an air. A feature counts when the game's state reaches it
    (parkmap.Reached)."""
    kinds = tuple(os.environ.get('PARK_KINDS', 'rail,ramp').split(','))
    entries = {f['id']: list(f['entries']) for f in r.plan.features}   # (Tour keeps only the run-ups it takes)
    t = parkmap.Tour(r.plan, kinds=kinds, least=12.0, rise=HOP_RISE)
    if not t.todo:
        # A dense level (a converted one: AI's Tutorial) has no 12 m run-up: half that
        for f in r.plan.features:
            f['entries'] = list(entries[f['id']])
        t = parkmap.Tour(r.plan, kinds=kinds, least=6.0, rise=HOP_RISE)
        r.say('no feature with a 12 m run-up: %d with 6 m' % len(t.todo))
    end = r.st.tick + ticks
    tries = collections.Counter()
    while r.st.tick < end:
        f = t.next(r.st.pos)
        if f is None:
            if r.park.steps:
                yield from circuit(r, end - r.st.tick)
            else:
                yield from wander(r, end, [e['on'] for es in entries.values() for e in es])
            return
        e = f['entries'][tries[f['id']] % len(f['entries'])]
        tries[f['id']] += 1
        what = '%s %s' % (f['id'], f['name'])
        t0, hits = r.st.tick, r.reached.hits[f['id']]
        ok = yield from goto(r, e['start'], near=4.0)
        if f['kind'] == 'rail':
            yield from rail_world(r, e['on'], e['end'], what, far=max(8.0, min(e['clear'], 26.0)), rise=e['rise'])
        else:
            yield from drive(r, e['on'], near=2.0, limit=300)
            yield from drive(r, e['through'], near=2.0, limit=240)
        got = r.reached.hits[f['id']] > hits
        t.result(f, got)
        r.tally('%s: %s' % (f['kind'], 'reached' if got else 'missed'))
        r.say('  %s: %s in %d ticks at %s score %.0f' % (what, 'reached' if got else 'missed', r.st.tick - t0,
                                                         [round(v, 1) for v in r.st.pos], r.st.run_score))


def zone_seconds(park, samples):
    """Seconds in each zone: the park's cells from a ride's positions (the world mapping inverted on a grid)."""
    out = {}
    for pos in samples:
        best = 'between'
        for name, a0, a1, b0, b1 in park.zones:
            c = park.world((a0 + a1) / 2, (b0 + b1) / 2)
            lo, hi = park.world(a0 - 0.5, b0 - 0.5), park.world(a1 - 0.5, b1 - 0.5)
            if min(lo[0], hi[0]) <= pos[0] <= max(lo[0], hi[0]) and min(lo[2], hi[2]) <= pos[2] <= max(lo[2], hi[2]):
                best = name
                break
        out[best] = out.get(best, 0) + 1
    return {k: round(v / 59.94) for k, v in sorted(out.items())}


GRINDING = (GRIND, STALL, LIP)


def play(name, park, read, cfg, enter, ticks, seed, out_dir, make_map=None, answer=None):
    """MAKE_MAP(r): the park's map once riding (parkmap.py), or None; RIDE=circuit rides the cell circuit instead of
    the map's tour, RIDE=rails the rail drill. ANSWER(r): a program for what the game asks between turns (BMX XXX's
    session over), or None while riding; the ride's program waits while it runs, the meter counts nothing."""
    meter = dm2.Meter()
    positions = []
    memory = []
    with core.Run(cfg) as run:
        r = Rider(run, park, seed)
        r.plan = r.reached = None
        r.sessions = []
        answering = [None]
        arena = memuse.Arena(run, 'dm2' if read is dm2.read else 'bmx') if os.environ.get('PARK_MEMORY') else None

        def program():
            ok = yield from enter(r)
            if not ok:
                return
            r.say('riding at %s forward %s' % ([round(v, 2) for v in r.st.pos], [round(v, 2) for v in r.st.fwd]))
            m = make_map(r) if make_map else None
            if m:
                parkmap.save(m, os.path.join(out_dir, name + '.parkmap'))
                # the run-ups the rider can reach from where it is (a map without a spawn otherwise takes its largest
                # joined floor, which in a converted level can be a roof: Tutorial's in DM2 had no run-up)
                r.plan = parkmap.Plan(m, run_up=20.0, clear=1.5, home=m.get('spawn') or tuple(r.st.pos))
                r.reached = parkmap.Reached(r.plan)
                r.say('map: %d cells, %d features (%s)' % (len(m['nodes']), len(m['features']), dict(collections.Counter(
                    f['kind'] for f in m['features']))))
            if arena:
                memory.append(dict(tick=r.st.tick, what='riding', **arena.sample()))
                r.say('memory: %s' % memory[-1])
            if os.environ.get('RIDE') == 'rails':
                yield from rail_drill(r, ticks)
            elif os.environ.get('RIDE') == 'circuit' or not r.plan:
                yield from circuit(r, ticks)
            else:
                yield from tour(r, ticks)
            r.say('ride over: score %.0f' % r.st.run_score)

        gen = program()
        menu_end = getattr(cfg, 'menu_end', 0)

        def policy(st):
            r.st = st
            if answer and answering[0] is None and st.script >= menu_end:
                answering[0] = answer(r)
            if answering[0] is not None:
                try:
                    return next(answering[0])
                except StopIteration:
                    answering[0] = None
                    meter.prev = meter.last_pos = None      # the rider put back at the start: no distance, no change
            meter.tick(st)
            if st.riding:
                positions.append(st.pos)
                if arena and memory and len(positions) % 3000 == 0:
                    memory.append(dict(tick=st.tick, what='ride', **arena.sample()))
                if r.reached:
                    r.reached.tick(st.pos, 'grind' if st.physics in GRINDING else 'air' if st.physics == AIR else None)
            if st.script < menu_end:
                return NOTHING
            try:
                return next(gen)
            except StopIteration:
                return None

        rec = core.Recorder()
        core.play(run, read, policy, max_ticks=ticks + 30000, recorder=rec)
        log_path = os.path.join(run.dir, 'run.log')
        # the ride as a pad script (the menu's, then the answers by script time): it replays the run tick for tick
        # with the same environment (a played A/B input: ab_inputs/bmx-pe-session.txt, career_ab.py)
        with open(os.path.join(run.dir, 'played-script.txt'), 'w') as f:
            f.write(cfg.script + ',' + rec.text())
    stats = meter.s.json()
    if memory:
        stats['memory'] = dict(arena=memory, gsmem=memuse.gsmem(log_path), heap=memuse.heap(log_path))
        print('memory', json.dumps(stats['memory']))
    stats['tricks_distinct'] = len(meter.tricks)
    if r.sessions:
        # each session's score (the game's run score starts again with each): the ride's is their sum
        stats['sessions'] = r.sessions + [r.st.run_score]
        stats['score'] = sum(stats['sessions'])
    stats['zones'] = zone_seconds(park, positions)
    stats['notes'] = r.notes
    if r.reached:
        stats.update(r.reached.summary())
        stats['track'] = r.reached.track
    os.makedirs(out_dir, exist_ok=True)
    core.save_json(os.path.join(out_dir, name + '.park.json'), stats)
    keys = ('ride_ticks', 'distance', 'rail', 'top_speed', 'air_ticks', 'jumps', 'grinds', 'c_grinds', 'c_tricks', 'tricks_started',
            'bails', 'c_bails', 'walls', 'lines_banked', 'best_line', 'score', 'sessions', 'zones')
    print(json.dumps({k: (round(stats[k], 2) if isinstance(stats.get(k), float) else stats.get(k)) for k in keys}))
    print('notes', sorted(r.notes.items(), key=lambda kv: -kv[1]))
    if r.reached:
        print('features', json.dumps(r.reached.summary()))


def ride_dm2(name, card, ticks, seed=1):
    """FREERIDE (main menu's third), rider and bike as they come, the modifiers, level select RIGHT x 8 (the park), the
    card frame: slot 1, the first park."""
    import dm2routes
    s = dm2routes.Script()
    plain = s.presses
    s.presses = lambda *b, after=dm2.MENU_GAP: plain(*b, after=after)
    s.presses(*['DOWN'] * dm2routes.MAIN['freeride'])
    s.press('CROSS', after=240)
    s.press('CROSS', after=240)        # the rider
    s.press('CROSS', after=240)        # the bike
    s.press('CROSS', after=240)        # the modifiers
    s.presses(*['RIGHT'] * int(os.environ.get('PARK_LEVEL_PRESSES', '8')))      # 8: the park
    run_dir = os.path.join(dm2.SCRATCH, 'runs', name)
    shutil.rmtree(run_dir, ignore_errors=True)
    shutil.copytree(card, os.path.join(run_dir, 'mc0'))
    cfg = dm2.run_config(name, s.text(), env={'PS2X_HEADLESS': '1'}, max_secs=max(900, ticks // 10), gs=SHOTS)
    cfg.menu_end = s.t + 30

    def enter(r):
        if SHOTS:
            print('shot', r.run.shot('levels'), flush=True)
        yield from tap('CROSS', gap=300)
        for k in range(12):
            if r.st.riding:
                break
            if SHOTS:
                print('shot', r.run.shot('load-%d' % k), flush=True)
            import dm2park
            r.say('load: mode %d frame kind %d step %d' % (r.st.mode, r.run.mem(dm2park.SYM['g_uiFrameKind'], 1)[0],
                                                          r.run.mem(dm2park.SYM['g_uiFrameStep'], 1)[0]))
            yield from tap('CROSS', gap=400)
        for _ in range(3000):
            if r.st.riding and r.st.frame > 120:
                break
            yield NOTHING
        if SHOTS:
            print('shot', r.run.shot('riding'), flush=True)
        r.say('riding:', r.st.riding, 'session %#x level %d' % (r.st.session, r.st.level))
        return r.st.riding

    play(name, DM2, dm2.read, cfg, enter, ticks, seed, os.path.join(dm2.SCRATCH, 'measure'),
         make_map=lambda r: parkmap.dm2_map(card, 0, name))


def ride_bmx(name, parks, ticks, seed=1):
    """PARK EDITOR (the main menu's third line, PS2X_BMX_PARK_EDITOR=1), SESSION (its third), the first park
    (bmxedit.py's ride_saved)."""
    import bmx
    import bmxroutes
    s = bmxroutes.Script()
    run_dir = os.path.join(bmx.SCRATCH, 'runs', name)
    shutil.rmtree(run_dir, ignore_errors=True)
    saves = run_dir + '-saves'
    shutil.rmtree(saves, ignore_errors=True)
    os.makedirs(os.path.join(saves, 'Parks'))
    for f in os.listdir(parks):
        if f.endswith('.sav'):
            shutil.copy(os.path.join(parks, f), os.path.join(saves, 'Parks', f))
    cfg = bmx.run_config(name, s.text(), env=dict(PS2X_EE_SCALE='4096', PS2X_BMX_PARK_EDITOR='1', PS2X_HEADLESS='1',
                                                  PS2X_SAVES_DIR=saves), max_secs=max(900, ticks // 10), gs=SHOTS)
    cfg.menu_end = s.t + 30

    def enter(r):
        for _ in range(3000):
            if r.st.mode == 1:
                break
            yield NOTHING
        yield from wait(240)
        yield from tap('DOWN', gap=40)
        yield from tap('DOWN', gap=40)
        yield from tap('CROSS', hold=8, gap=200)
        yield from tap('DOWN', gap=40)
        yield from tap('DOWN', gap=40)
        yield from tap('CROSS', hold=8, gap=300)       # SESSION: the parks saved
        if SHOTS:
            print('shot', r.run.shot('parks'), flush=True)
        yield from tap('CROSS', hold=8, gap=300)       # the first
        for _ in range(3000):
            if r.st.rider and r.st.mode == 2:
                break
            yield NOTHING
        for _ in range(40):
            if r.st.pos != (0.0, 0.0, 0.0):
                break
            yield from tap('CROSS', gap=40)
        if SHOTS:
            print('shot', r.run.shot('riding'), flush=True)
        r.say('riding:', r.st.riding, r.st.pos)
        return r.st.riding

    save = os.path.join(parks, sorted(f for f in os.listdir(parks) if f.endswith('.sav'))[0])
    play(name, BMX, bmx.read, cfg, enter, ticks, seed, os.path.join(bmx.SCRATCH, 'sessions'),
         make_map=lambda r: parkmap.zge3_park_map('bmx', save, name), answer=bmx_session_over)


def bmx_session_over(r):
    """BMX XXX's SESSION is a turn with a two-minute clock (round.cpp setRoundPhase: the session mode's turnSeconds):
    at 0 the turn is over, every rider is put away where it stands (its state frozen, the level's frame still
    counting) and the game over screen (screen_game_over.cpp) offers PLAY AGAIN first. Answered with CROSS (PLAY AGAIN,
    then the session's instructions) until the next turn runs, the rider at the park's start."""
    if not r.st.turn_over:
        return None
    return _bmx_play_again(r)


def _bmx_play_again(r):
    import bmx
    st = r.st
    t0, score = st.tick, st.run_score
    r.sessions.append(score)
    r.tally('session over')
    r.say('time up: session %d over (round phase %d, screen %d) at %s score %.0f' % (
        len(r.sessions), st.round_phase, st.screen, [round(v, 1) for v in st.pos], score))
    seen = []
    for k in range(60):
        st = r.st
        if (st.round_phase, st.screen) not in seen[-1:]:
            seen.append((st.round_phase, st.screen))
        if st.riding and st.round_phase == bmx.TURN_RUNNING and st.screen == 1 and k:
            break
        yield from tap('CROSS', hold=8, gap=90)
    r.say('riding again after %d ticks at %s (round phase, screen: %s)' % (
        r.st.tick - t0, [round(v, 1) for v in r.st.pos], ' '.join('%d,%d' % p for p in seen)))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 4 and a[0] == 'dm2':
        ride_dm2(a[1], a[2], int(a[3]), int(a[4]) if len(a) > 4 else 1)
    elif len(a) >= 4 and a[0] == 'bmx':
        ride_bmx(a[1], a[2], int(a[3]), int(a[4]) if len(a) > 4 else 1)
    else:
        print(__doc__)
