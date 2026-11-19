#!/usr/bin/env python3
"""Dave Mirra BMX 2: a played run that rides into the level's objects a goal or a touch handler wants (routes2, routes3),
on player_dm2's player with goalsteer's Approach: the level's paths while they lead there, then straight at the object
(a hop when the rider is held back), so a pane, a ladder or a boat off the paths is reached (routes1's dm2touch.py
went by the paths alone and stopped 8 units off the station glass). The level is left at the end by the game's own
menus (END RUN, YES; RUN FINISHED's EXIT TO MAIN MENU, YES), so its objects get their removed messages.

    dm2touch2.py NAME LEVEL MODE PLAN [TICKS [SEED]] [tier=T]

LEVEL is dm2map.LEVELS' index (the level select's), MODE proquest, session or freeride. PLAN is a comma list of steps:

    touch:PATTERN   every instance of the level's INS whose name matches PATTERN (a regular expression, any case),
                    nearest first, at the side of its model where a floor is at wheel height (routes3: a pane's face
                    is met by the wheels' probes, so a pane hung over the floor is out of reach): ridden to that floor,
                    then straight into the model, hopping as it comes (a pane's bottom a little over the wheels); done
                    when the rider's last touched object (Rider +0x1070, touchObject's) is one of the pattern's
                    objects (found on the engine's object list by their INS places), or the next after 3 tries
    touch1:PATTERN  as touch, until one of them is touched (the rest are not ridden to)
    npc             every NPC rider (g_npcs) while it stands at its replay's start: ridden into (riderTouchingNpc: within
                    1.5 of its drawn place, not moving away; in ProQuest the NPC screen comes up, CROSS)
    npchit          (routes3) an NPC ridden into while it rides or as its ride ends (npcCollision): the rider waits
                    beside its start (2.5 off, so a standing NPC is not met) until it rides, then parks on the start
                    itself; the NPC is put back there when its replay ends and the touch test finds the rider
    boat:PATTERN    (routes3) the moving objects of PATTERN (Swamp's fanboats) by their live places on the object
                    list: the rider waits at the shore point nearest each one's track and rides in as it passes
    ride:FRAMES     player_dm2's own riding
    exit            the level left: END RUN (the pause's last line: UP wraps to it), EXIT? YES, then RUN FINISHED's
                    EXIT TO MAIN MENU (UP wraps to it), YES; the run stops once the front end is back

tier=T (test only, as routes1's dm2touch.py): every rider's career record of the level put at tier T in the front end
(PS2X_POKE_SCRIPT), so ProQuest plays that tier's challenges (Woodward AMATEUR's ladders: tier 1; Trainyards PRO's
station glass: tier 2). sessions/NAME: play.log (each step, each object reached and how near), stats.json (dm2.Meter's
numbers, the steps), script.txt (the pad script).
"""
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import dm2  # noqa: E402
import goalsteer  # noqa: E402

NPCS, NPC_BYTES, NO_NPC = 3, 0x30, 0xFF     # game/dm2/game/npcs.h Npc
NPC_STATES, NPC_STANDING, NPC_MET, NPC_RIDING = 0x19, 0, 1, 2
NPC_DEFINITION, NPC_DEF_CHALLENGE = 0x28, 0x10
RIDER_POSITION, RIDER_ROOT_OFFSET = 0x1040, 0x1050  # the rider's position (dm2.read's pos), the drawn body's offset
RIDER_TOUCHED = 0x1070                      # rider.h touchedObject: the object last touched (touchObject)
OBJECT_POSITION = 0x10                      # zge SceneObject::position
NPC_TOUCH = 1.5                             # riderTouchingNpc's reach
# The menus' tables (game/dm2/overlay/screen_pause.cpp PauseChoice, screens.h GameOverLine)
PAUSE_CHOICES, PAUSE_EXIT_GAME = 6, 5
GAME_OVER_LINES, GAME_OVER_EXIT = 12, 8
SCREEN_NAME_ENTRY = 18                      # (dm2.py: a record's name, the keyboard)
WHEEL = 0.35                                # a wheel's centre over the floor (about)


def npcs(run, sym):
    data = run.mem(sym['g_npcs'], NPCS * NPC_BYTES)
    out = []
    for i in range(NPCS):
        d = data[i * NPC_BYTES:(i + 1) * NPC_BYTES]
        if d[0] == NO_NPC:
            break
        rider = run.u32(sym['g_riders'] + 4 * d[0])
        pos = drawn = None
        if 0x100000 <= rider < 0x2000000:
            pr = struct.unpack('<8f', run.mem(rider + RIDER_POSITION, 32))
            pos, drawn = pr[:3], tuple(a + b for a, b in zip(pr[:3], pr[4:7]))
        definition = struct.unpack_from('<I', d, NPC_DEFINITION)[0]
        challenge = run.mem(definition + NPC_DEF_CHALLENGE, 1)[0] if 0x100000 <= definition < 0x2000000 else None
        out.append(dict(index=i, state=d[NPC_STATES], sub=d[NPC_STATES + 1], pos=pos, drawn=drawn, challenge=challenge,
                        meetings=d[0x2C]))
    return out


def rider_drawn(run, st):
    pr = struct.unpack('<8f', run.mem(st.rider + RIDER_POSITION, 32))
    return tuple(a + b for a, b in zip(pr[:3], pr[4:7]))


def level_objects(level):
    """{name: dict(origin, lo, hi)} of the level's named INS instances: the origin and the world bounds of the model's
    vertices (every part). DM2's instances keep their name at +0x4C (dm2_level.goal_places)."""
    import dm2map
    import dm2_level as dl
    disc = dl.Disc(dm2map.DISC)
    geo = disc.read('ASSETS/LEVELS/%s/%s.GEO' % (level, level))
    ins = disc.read('ASSETS/LEVELS/%s/%s.INS' % (level, level))
    verts_of = {}
    for obj in dl.parse(geo)[0].kids:
        if obj.tag != b'TGEO':
            continue
        head = next(k for k in obj.kids if k.tag == b'TGHD')
        vs = []
        for part in (k for k in obj.kids if k.tag == b'TGPT'):
            streams = {k.tag: k.data for k in part.kids}
            if b'TGPH' in streams and b'TGVP' in streams:
                nv = struct.unpack_from('<H', streams[b'TGPH'], 4)[0]
                vs += [struct.unpack_from('<3f', streams[b'TGVP'], 12 * i) for i in range(nv)]
        verts_of[struct.unpack_from('<I', head.data, 0x18)[0]] = vs
    recs = [c.data for c in dl.walk(dl.parse(ins)) if c.tag == b'TGIH']
    by_id = {struct.unpack_from('<I', d, 0x48)[0]: d for d in recs}

    def place(d, v):
        for _ in range(16):
            v = tuple(a + b for a, b in zip(dl.quat_rotate(struct.unpack_from('<4f', d, 8), v), struct.unpack_from('<3f', d, 0x18)))
            d = by_id.get(struct.unpack_from('<I', d, 0x44)[0])
            if d is None:
                break
        return v

    out = {}
    for d in recs:
        name = d[0x4C:0x70].split(b'\0')[0].decode('latin-1')
        if not name:
            continue
        w = [place(d, v) for v in verts_of.get(struct.unpack_from('<I', d, 0x40)[0], [])]
        origin = place(d, (0.0, 0.0, 0.0))
        lo = tuple(min(p[i] for p in w) for i in range(3)) if w else origin
        hi = tuple(max(p[i] for p in w) for i in range(3)) if w else origin
        out[name] = dict(origin=origin, lo=lo, hi=hi)
    return out


class Floors:
    """Every collidable upward surface's height over a point (dm2_level.collision_triangles' faces), by 8-unit buckets."""

    def __init__(self, level):
        import dm2map
        import dm2_level as dl
        disc = dl.Disc(dm2map.DISC)
        groups = dl.collision_triangles(disc.read('ASSETS/LEVELS/%s/%s.GEO' % (level, level)),
                                        disc.read('ASSETS/LEVELS/%s/%s.INS' % (level, level)))
        self.cells = {}
        for x0, x1, z0, z1, tris in groups:
            for t in tris:
                xs, zs = [p[0] for p in t], [p[2] for p in t]
                for i in range(int(min(xs) // 8), int(max(xs) // 8) + 1):
                    for k in range(int(min(zs) // 8), int(max(zs) // 8) + 1):
                        self.cells.setdefault((i, k), []).append(t)

    def at(self, x, z):
        ys = []
        for a, b, c in self.cells.get((int(x // 8), int(z // 8)), ()):
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if abs(d) < 1e-9:
                continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) >= -1e-6:
                ys.append(l1 * a[1] + l2 * b[1] + l3 * c[1])
        return ys


def ways_in(obj, floors, level, main):
    """[(floor point, into point)]: floors beside the model (1.5 off its thinner side's faces, along its length) at which
    a wheel meets it (the model's height range holds the wheel, or is up to 0.8 over it: a hop), on the level map's main
    part; the into point is the model's middle across, at that floor's height."""
    lo, hi = obj['lo'], obj['hi']
    thin = 0 if hi[0] - lo[0] < hi[2] - lo[2] else 2
    along = 2 - thin
    out = []
    for side in (-1.0, 1.0):
        for f in (0.2, 0.35, 0.5, 0.65, 0.8):
            p = [0.0, 0.0, 0.0]
            mid = (lo[thin] + hi[thin]) / 2
            p[thin] = mid + side * (1.5 + (hi[thin] - lo[thin]) / 2)
            p[along] = lo[along] + f * (hi[along] - lo[along])
            for y in floors.at(p[0], p[2]):
                wheel = y - WHEEL
                if not (lo[1] - 0.2 <= wheel <= hi[1] + 0.8):
                    continue
                node = level.nearest((p[0], y, p[2]), radius=1)
                if node is None or level.part.get(node) != main or abs(level.world(node)[1] - y) > 0.6:
                    continue
                into = list(p)
                into[thin] = mid
                into[1] = y
                p2 = list(p)
                p2[1] = y
                out.append((tuple(p2), tuple(into), hi[1] + 0.3 < wheel, int(side > 0)))
    return out


def object_addresses(run, sym, places):
    """{place index: [object addresses]} of the engine's object list (g_objectList) whose position is one of places."""
    head = run.u32(sym['g_objectList'])
    found = {}
    node, n = head, 0
    while 0x100000 <= node < 0x2000000 and n < 20000:
        n += 1
        nxt, _, obj = struct.unpack('<3I', run.mem(node, 12))
        if 0x100000 <= obj < 0x2000000:
            p = struct.unpack('<3f', run.mem(obj + OBJECT_POSITION, 12))
            for i, q in enumerate(places):
                if abs(p[0] - q[0]) < 0.05 and abs(p[1] - q[1]) < 0.05 and abs(p[2] - q[2]) < 0.05:
                    found.setdefault(i, []).append(obj)
        node = nxt
    return found, n


LEVEL_OBJECTS, LEVEL_OBJECT_BYTES, NAME_BYTES = 0x100, 0x14, 0x10   # zge/scene/objects.cpp: DM2's pool, LevelObject, LevelObjectName


def named_objects(run, sym, names):
    """{name: [scene object addresses]} of the level objects with those names: the engine's object list is its
    LevelObject records (levelObjectsBegin's pool), and record i's name hash is the i-th LevelObjectName's after it."""
    import dm2_level as dl
    want = {dl.name_hash(n): n for n in names}
    begin = sym['levelObjectsBegin']
    names_at = begin + LEVEL_OBJECTS * LEVEL_OBJECT_BYTES
    out = {}
    node, n = run.u32(sym['g_objectList']), 0
    while 0x100000 <= node < 0x2000000 and n < 4096:
        n += 1
        nxt, _, obj = struct.unpack('<3I', run.mem(node, 12))
        i = (node - begin) // LEVEL_OBJECT_BYTES
        if 0 <= i < LEVEL_OBJECTS and 0x100000 <= obj < 0x2000000:
            h = run.u32(names_at + i * NAME_BYTES + 8)
            if h in want:
                out.setdefault(want[h], []).append(obj)
        node = nxt
    return out


def all_objects(run, sym):
    """[(address, position)] of every object on the engine's list."""
    head = run.u32(sym['g_objectList'])
    out = []
    node, n = head, 0
    while 0x100000 <= node < 0x2000000 and n < 20000:
        n += 1
        nxt, _, obj = struct.unpack('<3I', run.mem(node, 12))
        if 0x100000 <= obj < 0x2000000:
            out.append((obj, struct.unpack('<3f', run.mem(obj + OBJECT_POSITION, 12))))
        node = nxt
    return out


def play(name, level, mode, plan, ticks=9000, seed=1, tier=None, max_secs=2400):
    import dm2map
    import player_dm2 as pd
    sym = goalsteer.Symbols('dm2')
    level_name = dm2map.LEVELS[level]
    menu = dm2.menu_script(mode, level)
    d = dm2.session_dir(name)
    os.makedirs(d, exist_ok=True)
    env = {}
    if tier:
        import dm2routes
        # (TIER_TICKS: the vsyncs of the writes, '/' between; 3700 is in the level select)
        # The challenge is not the record's +0x3D as such: each run's restartLevelGoals (game/goals.cpp) sets it to the
        # first challenge with a goal not done (LevelGoals: 5 challenges of 4 goals, 3 bytes each, kGoalDone 0x40 in
        # the first). So the tiers below T get their goals marked done (words 0x40000040, 0x00400000, 0x00004000 of a
        # challenge's 12 bytes), as a career that played them has them.
        done = (0x40000040, 0x00400000, 0x00004000)
        pokes = []
        for t in os.environ.get('TIER_TICKS', '3700').split('/'):
            for rider in range(21):
                base = dm2routes.CAREER + rider * 0x3C8 + 0x13C + dm2routes.LEVEL_BLOCKS[level] * 0x44
                for below in range(tier):
                    pokes += ['%x:%x=%x' % (int(t), base + below * 12 + 4 * w, done[w]) for w in range(3)]
                pokes.append('%x:%x=%x' % (int(t), base + 0x3C, 0x0001 | tier << 8))
        env['PS2X_POKE_SCRIPT'] = ','.join(pokes)
    for kv in filter(None, os.environ.get('PLAY_ENV', '').split(';')):
        k, v = kv.split('=', 1)
        env[k] = v
    cfg = dm2.run_config(name, menu.text(), env=env, max_secs=max_secs, gs=os.environ.get('GS') == '1')
    meter, log = dm2.Meter(), pd.Log(os.path.join(d, 'play.log'))
    lv = dm2map.Level(level_name)
    plain_path = lv.path
    lv.path = lambda a, b, limit=400000: plain_path(a, b, limit)     # (the longer search: Woodward's paths run long)
    main = max(range(len(lv.sizes)), key=lambda n: lv.sizes[n])
    objects = level_objects(level_name)
    floors = Floors(level_name)
    steps = [dict(kind=x.split(':', 1)[0], arg=x.split(':', 1)[1] if ':' in x else '') for x in plan.split(',') if x]
    s = dict(program=[], results=[], reached=[], met=0, screens=[], touched=[], collisions=0, npc_log={})
    box = {}

    def touched_now(st):
        return box['run'].u32(st.rider + RIDER_TOUCHED) if getattr(st, 'rider', 0) else 0

    class Into(pd.Task):
        """Straight from a floor beside the model into it (its middle across), a hop as it comes when hop is set; done
        once the rider's last touched object is one of `want`, or after its frames."""
        name = 'into'

        def __init__(self, player, start, into, want, hop, label, reach):
            super().__init__(player)
            self.start, self.into, self.want, self.hop, self.label = start, into, want, hop, label
            self.reach = reach
            self.limit = 150
            self.hopped = 0
            self.done = False
            # aim past the middle, along the line from the floor point
            dx, dz = into[0] - start[0], into[2] - start[2]
            n = math.hypot(dx, dz) or 1.0
            self.aim = (into[0] + dx / n * 4.0, into[1], into[2] + dz / n * 4.0)

        def step(self, st):
            self.frames += 1
            if touched_now(st) in self.want:
                self.done = True
                self.p.log(st, 'into %s: touched (the rider stands on it)' % self.label)
                return None
            # at the model: through its middle (a pane broken: the touch lets the rider through) or against its face
            # (a solid one), on the floor at wheel height (touchObject clears touchedObject at each touch of the
            # ground's faces, so a wall's touch is not left there to read)
            if goalsteer.flat(st.pos, self.into) < self.reach and abs(st.pos[1] - self.into[1]) < 1.0:
                self.done = True
                self.p.log(st, 'into %s: at the model, %.1f from its middle' % (self.label, goalsteer.flat(st.pos, self.into)))
                return None
            if self.frames > self.limit:
                return None
            lx, err = pd.steer(st, self.aim, gain=2.0)
            left = goalsteer.flat(st.pos, self.into)
            if self.frames % 10 == 1:
                self.p.log(st, 'into %s: %.1f to the middle, heading err %.2f, touched %x' % (self.label, left, err, touched_now(st)))
                if os.environ.get('GS') == '1' and self.frames in (1, 11, 21):
                    self.p.log(st, 'shot', box['run'].shot('into-%s-%d' % (self.label, st.script)))
            if self.hop and left < 3.2 and self.hopped < 20 and st.physics == pd.GROUND:
                self.hopped += 1
                return core.pad('UP', pd.HOP, lx=lx) if self.hopped > 6 else core.pad('UP', lx=lx)
            if self.hop and self.hopped and self.hopped < 20:
                self.hopped += 1
            return core.pad('UP', lx=lx)

    class Hold(pd.Task):
        """Still at a place: pedal back to it when off by more than `near`, else the stick back (the brake)."""
        name = 'hold'

        def __init__(self, player, where, near, frames, until=None):
            super().__init__(player)
            self.where, self.near, self.limit, self.until = where, near, frames, until

        def step(self, st):
            self.frames += 1
            if self.frames > self.limit or (self.until and self.until(st)):
                return None
            p = self.where(st)
            if p is None:
                return None
            d = goalsteer.flat(st.pos, p)
            lx, err = pd.steer(st, p)
            if d > self.near:
                if st.speed > 0.06:
                    return core.pad(lx=lx)
                return core.pad('UP', lx=lx)
            return core.NOTHING     # (coasting to a stop: DM2 has no brake; the stick back or DOWN is a manual)

    class TouchPlayer(pd.Player):
        step_i = 0
        step_frames = 0

        def ride(self, st):
            # (an approach handles its own hold-ups: hops, rounding; the player's stuck turn would end it)
            if isinstance(self.task, (goalsteer.Approach, Hold, Into)):
                self.slow = 0
            return super().ride(st)

        def finish(self, st, result):
            step = steps[self.step_i]
            s['results'].append('%s:%s %s' % (step['kind'], step['arg'], result))
            self.log(st, 'step %s:%s %s' % (step['kind'], step['arg'], result))
            self.step_i += 1
            self.step_frames = 0

        def touch_step(self, st, step, only_one):
            run = box['run']
            kind, arg = step['kind'], step['arg']
            if step.get('todo') is None:
                pat = re.compile(arg, re.I)
                names = sorted(n for n in objects if pat.search(n))
                places = [objects[n]['origin'] for n in names]
                found, listed = object_addresses(run, sym, places)
                step['want'] = {a for i in found for a in found[i]}
                todo = []
                for i, n in enumerate(names):
                    ways = ways_in(objects[n], floors, lv, main)
                    if ways:
                        todo.append((n, ways))
                    else:
                        s['reached'].append('%s no floor at wheel height' % n)
                todo.sort(key=lambda nw: min(goalsteer.flat(st.pos, w[0]) for w in nw[1]))
                step['todo'] = todo
                self.log(st, '%s %s: %d objects, %d on the object list (%d listed), ways in: %s' % (
                    kind, arg, len(names), len(found), listed, '; '.join('%s %d (%.1f %.1f %.1f)' % (n, len(w), *w[0][0]) for n, w in todo)))
            todo = step['todo']
            last = step.pop('last', None)
            if last is not None:
                n, task, ways = last
                done = getattr(task, 'done', False)
                tries = step.setdefault('tries', {})
                tries[n] = tries.get(n, 0) + 1
                if done:
                    s['reached'].append('%s touched' % n)
                    s['touched'].append(n)
                    meter.s.count('touched')
                    self.log(st, 'touched %s' % n)
                    if only_one:
                        todo.clear()
                elif isinstance(task, goalsteer.Approach) and task.reached:
                    todo.insert(0, (n, ways))
                    step['into'] = True
                elif tries[n] < int(os.environ.get('TOUCH_TRIES', '4')):
                    todo.insert(0, (n, ways))                       # another way in (the other side first)
                else:
                    s['reached'].append('%s closest %.1f' % (n, getattr(task, 'closest', -1)))
            if touched_now(st) in step['want'] and not only_one:
                pass
            if not todo:
                return 'done'
            n, ways = todo.pop(0)
            # the try's way in: the nearest of one side, the sides in turn (a pane's faces may be one-sided)
            k = step.setdefault('tries', {}).get(n, 0)
            sides = [w for w in ways if w[3] == (k // 2) % 2] or ways
            way = min(sides, key=lambda w: goalsteer.flat(st.pos, w[0]))
            if step.pop('into', False) and goalsteer.flat(st.pos, way[0]) < 3.0:
                o = objects[n]
                half = min(o['hi'][0] - o['lo'][0], o['hi'][2] - o['lo'][2]) / 2
                task = Into(self, way[0], way[1], step['want'], way[2] or os.environ.get('TOUCH_HOP') == '1', n, half + 0.9)
                self.log(st, 'into %s from (%.1f %.1f %.1f)%s' % (n, *way[0], ', hopping' if task.hop else ''))
            else:
                task = goalsteer.Approach(self, lambda st2, p=way[0]: p, near=1.2, rise=1.5, limit=3000, label=n)
            step['last'] = (n, task, ways)
            return task

        def next_task(self, st):
            run = box['run']
            while self.step_i < len(steps):
                step = steps[self.step_i]
                kind, arg = step['kind'], step['arg']
                if kind == 'exit':
                    return None
                if kind == 'ride':
                    if self.step_frames > int(arg or '600'):
                        self.finish(st, 'ridden')
                        continue
                    break
                if self.step_frames > int(step.get('limit', os.environ.get('STEP_LIMIT', 12000))):
                    self.finish(st, 'given up (time)')
                    continue
                if kind in ('touch', 'touch1'):
                    r = self.touch_step(st, step, kind == 'touch1')
                    if r == 'done':
                        self.finish(st, 'done, touched %d' % len([t for t in s['touched']]))
                        continue
                    return r
                if kind == 'npc':
                    here = npcs(run, sym)
                    if step.get('logged') is None:
                        step['logged'] = True
                        self.log(st, 'npcs: %s' % '; '.join('%d state %d/%d challenge %s at %s' % (
                            n['index'], n['state'], n['sub'], n['challenge'], '(%.0f %.0f %.0f)' % n['pos'] if n['pos'] else '?') for n in here))
                    tries = step.setdefault('tries', {})
                    want = int(arg) if arg else None
                    left = [n for n in here if tries.get(n['index'], 0) < 6 and (want is None or n['index'] == want)]
                    if not left or s['met'] >= (1 if want is not None else len(here)):
                        self.finish(st, 'met %d of %d' % (s['met'], len(here)))
                        continue
                    for n in here:
                        if n['state'] == NPC_STANDING and n['drawn']:
                            s['npc_log'][n['index']] = n['drawn']
                    standing = [n for n in left if n['state'] == NPC_STANDING and n['pos']]
                    if not standing:
                        if want is not None and want in s['npc_log']:
                            # it rides: beside its start (3 off) until it is back and standing
                            p0 = s['npc_log'][want]
                            a = math.atan2(st.pos[2] - p0[2], st.pos[0] - p0[0])
                            side = (p0[0] + math.cos(a) * 3.0, p0[1], p0[2] + math.sin(a) * 3.0)
                            if goalsteer.flat(st.pos, side) > 4.0:
                                return goalsteer.Approach(self, lambda st2, p=side: p, near=2.0, limit=1500, slow=0.15,
                                                          label='by npc %d start' % want)
                            return Hold(self, lambda st2, p=side: p, 1.5, 120,
                                        until=lambda st2: [m for m in npcs(run, sym) if m['index'] == want][0]['state'] == NPC_STANDING)
                        break
                    n = min(standing, key=lambda n: goalsteer.flat(st.pos, n['pos']))
                    tries[n['index']] = tries.get(n['index'], 0) + 1

                    def live(st2, index=n['index']):
                        now = [m for m in npcs(run, sym) if m['index'] == index]
                        return now[0]['drawn'] if now and now[0]['state'] == NPC_STANDING else None
                    return goalsteer.Approach(self, live, near=0.9, rise=1.4, limit=3000, slow=0.18, label='npc %d' % n['index'])
                if kind == 'npchit':
                    return self.npchit_step(st, step)
                if kind == 'npcpath':
                    return self.npcpath_step(st, step)
                if kind == 'boat':
                    return self.boat_step(st, step)
                if kind == 'boatpath':
                    return self.boatpath_step(st, step)
                self.finish(st, 'unknown step')
            return super().next_task(st)

        def npchit_step(self, st, step):
            """Beside an NPC's start while it stands; on the start once it rides; the collision as it is put back."""
            run = box['run']
            if s['collisions'] >= 1:
                self.finish(st, 'collisions %d' % s['collisions'])
                return self.next_task(st)
            here = npcs(run, sym)
            if not here:
                self.finish(st, 'no npcs')
                return self.next_task(st)
            if 'npc' not in step:
                # the nearest NPC that stands (its start is where it stands)
                standing = [n for n in here if n['state'] in (NPC_STANDING, NPC_MET) and n['drawn'] and
                            (not step['arg'] or n['index'] == int(step['arg']))]
                if not standing:
                    return Hold(self, lambda st2: st.pos, 3.0, 60)
                n = min(standing, key=lambda n: goalsteer.flat(st.pos, n['drawn']))
                step['npc'], step['start'] = n['index'], n['drawn']
                self.log(st, 'npchit: npc %d, its start (%.1f %.1f %.1f)' % (n['index'], *n['drawn']))
            index, start = step['npc'], step['start']
            now = [m for m in here if m['index'] == index][0]
            if now['state'] in (NPC_STANDING, NPC_MET):
                # 6 off its start (out of the touch's 1.5 and the meeting's 4) until it rides
                if 'beside' not in step:
                    # NPCHIT_OFF units from its start towards the rider (the meeting's place); NPCHIT_AXIS=1: along x
                    # or z only, the one the rider is further along (a narrow ledge, as Trainyards' first NPC's)
                    off = float(os.environ.get('NPCHIT_OFF', '6'))
                    dx, dz = st.pos[0] - start[0], st.pos[2] - start[2]
                    if os.environ.get('NPCHIT_AXIS') == '1':
                        dx, dz = (math.copysign(1, dx), 0.0) if abs(dx) > abs(dz) else (0.0, math.copysign(1, dz))
                    n = math.hypot(dx, dz) or 1.0
                    step['beside'] = (start[0] + dx / n * off, start[1], start[2] + dz / n * off)
                    self.log(st, 'npchit: waiting at (%.1f %.1f %.1f)' % step['beside'])
                if goalsteer.flat(st.pos, step['beside']) > 6.0:
                    return goalsteer.Approach(self, lambda st2, p=step['beside']: p, near=2.0, limit=2000, slow=0.15, label='by npc %d' % index)
                return Hold(self, lambda st2, p=step['beside']: p, 1.0, 600,
                            until=lambda st2: [m for m in npcs(run, sym) if m['index'] == index][0]['state'] not in (NPC_STANDING, NPC_MET))
            # it rides (or bails): on its start, still, for its ride's end (kNpcRideOver puts it back there and tests
            # the touch while it turns to face its replay's start)
            if goalsteer.flat(st.pos, start) > 1.2:
                return goalsteer.Approach(self, lambda st2, p=start: p, near=0.5, rise=1.5, limit=900, slow=0.12, label='npc %d start' % index)
            return Hold(self, lambda st2, p=start: p, 0.5, 200)

        def npcpath_step(self, st, step):
            """An NPC's ride watched once (its drawn place every frame of it: the replay is the same each ride), then
            the rider waits still on a ground point of that ride's way (90 frames or more in, on the map's main part)
            until the NPC's next ride runs into it (riderTouchingNpc: within 1.5, closing: npcCollision)."""
            run = box['run']
            index = int(step['arg'] or 0)
            if s['collisions'] >= 1:
                self.finish(st, 'collisions %d' % s['collisions'])
                return self.next_task(st)
            way = s.get('npc_way', {}).get(index)
            if not way or 'point' not in step:
                if way:
                    ok = []
                    for t, p, drawn in way:
                        if t < 90 or t % 4:
                            continue
                        ys = floors.at(p[0], p[2])
                        if not any(0.1 < y - p[1] < 0.7 for y in ys):
                            continue
                        node = lv.nearest((p[0], max(y for y in ys if y - p[1] < 0.7), p[2]), radius=1)
                        if node is None or lv.part.get(node) != main:
                            continue
                        ok.append((t, p, drawn))
                    if ok:
                        t, p, drawn = min(ok, key=lambda o: goalsteer.flat(st.pos, o[1]) + o[0] * 0.02)
                        step['point'] = p
                        self.log(st, 'npcpath: npc %d ride of %d frames, %d ground points; waiting at (%.1f %.1f %.1f), %d frames in' % (
                            index, way[-1][0], len(ok), *p, t))
                    else:
                        self.log(st, 'npcpath: no ground point on npc %d way' % index)
                        s['npc_way'].pop(index)
                # (meanwhile towards the NPC, 20 off: its next ride is not long after)
                here = [m for m in npcs(run, sym) if m['index'] == index]
                if here and here[0]['drawn'] and goalsteer.flat(st.pos, here[0]['drawn']) > 30.0 and not way:
                    return goalsteer.Approach(self, lambda st2, q=here[0]['drawn']: q, near=20.0, limit=600, label='towards npc %d' % index)
                return Hold(self, lambda st2: st2.pos, 5.0, 60)
            p = step['point']
            if goalsteer.flat(st.pos, p) > 6.0:
                return goalsteer.Approach(self, lambda st2, p=p: p, near=2.0, limit=2500, slow=0.15, label='npc %d way' % index)
            return Hold(self, lambda st2, p=p: p, 0.8, 300)

        def boatpath_step(self, st, step):
            """(Swamp's fanboats loop on their paths) every moving object's track for TRACK frames, then the rider waits
            still on the track point nearest it that is on the map's main part (shallow water is ridden), until the
            object passes within 1.5 (its hull meets the wheels' probes: the touch), then the next object."""
            run = box['run']
            if 'tracks' not in step:
                if 'look' not in step:
                    step['look'] = dict(all_objects(run, sym))
                    step['t0'] = st.script
                    return Hold(self, lambda st2: st2.pos, 5.0, 60)
                now = dict(all_objects(run, sym))
                step['movers'] = [a for a, p in now.items() if a in step['look'] and math.dist(p, step['look'][a]) > 0.5]
                if step['arg']:
                    # the named ones (the level objects' names), moving or not
                    pat = re.compile(step['arg'], re.I)
                    found = named_objects(run, sym, [n for n in objects if pat.search(n)])
                    self.log(st, 'boatpath: named %s' % ', '.join('%s %s' % (n, ' '.join('%x' % a for a in v)) for n, v in sorted(found.items())))
                    step['movers'] = [a for v in found.values() for a in v]
                step['tracks'] = {a: [] for a in step['movers']}
                step['done'] = []
                self.log(st, 'boatpath: %d moving objects' % len(step['movers']))
            if st.script - step['t0'] < int(os.environ.get('TRACK', '1200')):
                return Hold(self, lambda st2: st2.pos, 5.0, 30)
            if 'point' not in step:
                # the busiest place of the tracks (the boats take a junction's exits at random: no one loop), counted
                # in 3-unit cells, on the map's main part, nearer ones first
                cells = {}
                for a, tr in step['tracks'].items():
                    for p in tr:
                        cells.setdefault((int(p[0] // 3), int(p[2] // 3)), []).append((a, p))
                best = None
                for key, hits in cells.items():
                    p = hits[0][1]
                    ys = [y for y in floors.at(p[0], p[2]) if -1.5 < y - p[1] < 2.0]
                    if not ys:
                        continue
                    node = lv.nearest((p[0], min(ys), p[2]), radius=1)
                    if node is None or lv.part.get(node) != main:
                        continue
                    score = len(hits) * len({h[0] for h in hits}) / (1.0 + goalsteer.flat(st.pos, p) / 150.0)
                    if best is None or score > best[0]:
                        best = (score, p, len(hits), len({h[0] for h in hits}))
                if os.environ.get('BOAT_AT'):
                    # (a place given: X,Z, at the tracks' height there)
                    x, z = map(float, os.environ['BOAT_AT'].split(','))
                    near = [p for tr in step['tracks'].values() for p in tr if goalsteer.flat(p, (x, 0, z)) < 3.0]
                    best = (0, (x, near[0][1] if near else 4.0, z), len(near), 0)
                if best is None:
                    self.finish(st, 'no track point to wait at')
                    return self.next_task(st)
                step['point'], step['since'], step['closest'] = best[1], st.script, 1e9
                self.log(st, 'boatpath: waiting at (%.1f %.1f %.1f): %d samples of %d objects there, %.1f off' % (
                    *best[1], best[2], best[3], goalsteer.flat(st.pos, best[1])))
            p = step['point']
            me = rider_drawn(run, st)
            near = min((math.dist(me, struct.unpack('<3f', run.mem(a + OBJECT_POSITION, 12))), a) for a in step['tracks'])
            step['closest'] = min(step['closest'], near[0])
            if near[0] < 1.5:
                self.log(st, 'boatpath: object %x met, %.1f off' % (near[1], near[0]))
                meter.s.count('boats_met')
                step['met'] = step.get('met', 0) + 1
                if step['met'] >= int(os.environ.get('BOATS', '1')):
                    self.finish(st, 'met %d' % step['met'])
                    return self.next_task(st)
                return Hold(self, lambda st2: st2.pos, 5.0, 60)
            if st.script - step['since'] > int(os.environ.get('BOAT_WAIT', '4000')):
                self.finish(st, 'not met, closest %.1f' % step['closest'])
                return self.next_task(st)
            if goalsteer.flat(st.pos, p) > 3.0:
                return goalsteer.Approach(self, lambda st2, p=p: p, near=1.5, limit=1500, slow=0.15, label='boat place')
            return Hold(self, lambda st2, p=p: p, 0.8, 10)

        def boat_step(self, st, step):
            """The named moving objects (the level objects' names: Swamp's fanboats) chased by their live places, led by
            their motion (the place they reach while the rider rides there); the nearest first; met within 1.5."""
            run = box['run']
            if 'boats' not in step:
                pat = re.compile(step['arg'], re.I)
                found = named_objects(run, sym, [n for n in objects if pat.search(n)])
                step['boats'] = {n: v[0] for n, v in found.items()}
                step['tries'] = {}
                self.log(st, 'boats: %s' % ', '.join('%s %x' % kv for kv in sorted(step['boats'].items())))
                if not found:
                    self.finish(st, 'none on the object list')
                    return self.next_task(st)
            if step.get('met', 0) >= int(os.environ.get('BOATS', '1')):
                self.finish(st, 'met %d' % step['met'])
                return self.next_task(st)
            left = {n: a for n, a in step['boats'].items() if step['tries'].get(n, 0) < 2}
            if not left:
                self.finish(st, 'met %d' % step.get('met', 0))
                return self.next_task(st)
            where = {n: struct.unpack('<3f', run.mem(a + OBJECT_POSITION, 12)) for n, a in left.items()}
            n = min(where, key=lambda k: goalsteer.flat(st.pos, where[k]))
            step['tries'][n] = step['tries'].get(n, 0) + 1
            addr = step['boats'][n]
            self.log(st, 'boat %s at (%.1f %.1f %.1f), %.1f off' % (n, *where[n], goalsteer.flat(st.pos, where[n])))
            last = {}

            def led(st2, a=addr):
                q = struct.unpack('<3f', run.mem(a + OBJECT_POSITION, 12))
                prev = last.get('q')
                last['q'] = q
                if prev is None or math.dist(prev, q) > 3.0:      # (a jump: the path's end, on from its start)
                    return q
                v = tuple(x - y for x, y in zip(q, prev))
                k = min(goalsteer.flat(st2.pos, q) / max(st2.speed, 0.2), 90.0)
                return tuple(x + vx * k for x, vx in zip(q, v))

            class Chase(goalsteer.Approach):
                def step(self2, st2):
                    q = struct.unpack('<3f', run.mem(addr + OBJECT_POSITION, 12))
                    d = goalsteer.flat(st2.pos, q)
                    self2.closest_boat = min(getattr(self2, 'closest_boat', 1e9), d)
                    if d < 1.5:
                        step['met'] = step.get('met', 0) + 1
                        meter.s.count('boats_met')
                        self2.p.log(st2, 'boat %s met, %.1f off (%.1f in height)' % (n, d, st2.pos[1] - q[1]))
                        return None
                    r = super().step(st2)
                    if r is None:
                        self2.p.log(st2, 'boat %s: closest %.1f' % (n, self2.closest_boat))
                    return r
            return Chase(self, led, near=0.5, limit=int(os.environ.get('BOAT_CHASE', '1200')), label=n)

    pl = TouchPlayer(lv, seed, log)
    pl.explorer = mode == 'freeride'
    pl.line_share = 0.5

    def exit_step(st):
        """The level left by the game's menus, closed on its own tables (the collection adds lines after the game's
        last, so UP's wrap is not END RUN): START until paused; the pause's Exit Game line (g_pauseChoices), EXIT? YES;
        then the game over's EXIT (g_gameOverLines), EXIT? YES; the run stops once the front end is back."""
        k = s['ek'] = s.get('ek', 0) + 1
        if k > 6000:
            log(st, 'exit: no front end')
            return None
        g = s['exit']
        if st.mode == 1:
            s['fe'] = s.get('fe', 0) + 1
            if s['fe'] == 1:
                log(st, 'front end: the level left')
                meter.s.count('level_left')
            return None if s['fe'] > 300 else core.NOTHING
        if g == 'pause':
            if st.mode == 3:
                choices = list(run.mem(sym['g_pauseChoices'], PAUSE_CHOICES))
                if PAUSE_EXIT_GAME in choices:
                    log(st, 'exit: pause choices %s' % choices)
                    s['exit'], s['ek'] = 'over', 0
                    s['program'] = bmx_presses(120, *['DOWN'] * choices.index(PAUSE_EXIT_GAME), 'CROSS', 120, 'DOWN', 30, 'CROSS', 300)
                    return s['program'].pop(0)
                return core.NOTHING
            return core.pad('START') if k % 120 < 6 else core.NOTHING
        if g == 'over':
            screen = run.mem(sym['g_gameScreen'], 1)[0]
            if screen == SCREEN_NAME_ENTRY and s.get('named') != s.get('ek_screen_at'):
                # a record's name (the keyboard, dm2.py's way): END, the grid's bottom right corner
                s['named'] = s.get('ek_screen_at')
                log(st, 'exit: a record\'s name, END')
                s['program'] = bmx_presses(60, *['RIGHT'] * 11, *['DOWN'] * 4, 'CROSS', 240)
                return s['program'].pop(0)
            lines = struct.unpack('<%dH' % GAME_OVER_LINES, run.mem(sym['g_gameOverLines'], 2 * GAME_OVER_LINES))
            if k > 300 and GAME_OVER_EXIT in lines and k % 300 == 0:
                log(st, 'exit: game over lines %s' % (lines,))
                s['exit'] = 'confirm'
                s['program'] = bmx_presses(60, *['DOWN'] * lines.index(GAME_OVER_EXIT), 'CROSS', 120, 'DOWN', 30, 'CROSS', 300)
                return s['program'].pop(0)
            # the run's screens before it (ProQuest's results, a record's name): CONTINUE
            return core.pad('CROSS') if k > 300 and GAME_OVER_EXIT not in lines and k % 300 == 150 else core.NOTHING
        # (after the exit's YES) the front end is coming; a CROSS now and then for a screen on the way
        return core.pad('CROSS') if k % 300 == 150 else core.NOTHING

    def bmx_presses(*items):
        out = []
        for it in items:
            out += [core.NOTHING] * it if isinstance(it, int) else [core.pad(it)] * 6 + [core.NOTHING] * 30
        return out

    def watch(st):
        """Each frame of play: the NPCs' states (a meeting, a collision: an NPC put to bailing, or the rider's bail of
        kind 3) and what the rider touched."""
        run = box['run']
        if st.script % 4 == 0 or st.physics == pd.BAIL:
            here = npcs(run, sym)
            prev = s.get('npc_states')
            now = [(n['state'], n['sub']) for n in here]
            if prev is not None and now != prev:
                for n, (a, b) in zip(here, prev):
                    if (n['state'], n['sub']) != (a, b):
                        log(st, 'npc %d state %d/%d -> %d/%d at %s, %.1f off' % (
                            n['index'], a, b, n['state'], n['sub'], '(%.1f %.1f %.1f)' % n['drawn'] if n['drawn'] else '?',
                            math.dist(rider_drawn(run, st), n['drawn']) if n['drawn'] else -1))
                        if n['state'] == 3 and a != 3 and n['drawn'] and math.dist(rider_drawn(run, st), n['drawn']) < 3.0:
                            s['collisions'] += 1
                            meter.s.count('npc_collisions')
            s['npc_states'] = now
        # the moving objects' tracks (boatpath)
        for k in steps:
            if k['kind'] == 'boatpath' and 'tracks' in k and st.script % 4 == 0 and st.script - k['t0'] < int(os.environ.get('TRACK', '1200')):
                for a, tr in k['tracks'].items():
                    tr.append(struct.unpack('<3f', run.mem(a + OBJECT_POSITION, 12)))
        # each NPC's first ride, frame by frame (npcpath)
        for n in npcs(run, sym) if st.script % 1 == 0 and any(k['kind'] == 'npcpath' for k in steps) else ():
            ways = s.setdefault('npc_way', {})
            rec = s.setdefault('npc_rec', {})
            if n['state'] == NPC_RIDING and n['index'] not in ways:
                r = rec.setdefault(n['index'], [])
                r.append((len(r), n['pos'], n['drawn']))
            elif n['index'] in rec and n['index'] not in ways and rec[n['index']]:
                ways[n['index']] = rec.pop(n['index'])
        if st.physics == pd.BAIL and st.bail_kind == 3 and not s.get('bail3') and any(
                n['drawn'] and math.dist(rider_drawn(run, st), n['drawn']) < 3.0 for n in npcs(run, sym)):
            s['bail3'] = True
            s['collisions'] += 1
            meter.s.count('npc_collisions')
            log(st, 'bail: ran into an NPC (kind 3)')
        if st.physics != pd.BAIL:
            s['bail3'] = False
        t = touched_now(st)
        if t and t != s.get('last_touched'):
            log(st, 'touched object %x' % t)
        s['last_touched'] = t

    def policy(st):
        meter.tick(st)
        if s.get('exit') and st.frame:
            screen = run.mem(sym['g_gameScreen'], 1)[0]
            if (st.mode, screen) != s.get('exit_seen'):
                s['exit_seen'] = (st.mode, screen)
                s['ek_screen_at'] = st.script
                log(st, 'exit: mode %d screen %d' % (st.mode, screen))
                if os.environ.get('GS') == '1':
                    log(st, 'shot', run.shot('exit-%d-%d-%d' % (st.script, st.mode, screen)))
        if s['program']:
            return s['program'].pop(0)
        if s.get('exit'):
            return exit_step(st)
        if 't' in s and not st.riding:
            # a screen over the play (the NPC's, a challenge's, RUN FINISHED): CROSS now and then
            k = s['away'] = s.get('away', 0) + 1
            if k == 1:
                screen = run.mem(sym['g_gameScreen'], 1)[0]
                s['screens'].append(screen)
                log(st, 'off the play: mode %d screen %d' % (st.mode, screen))
                if any(n['state'] == NPC_MET for n in npcs(run, sym)):
                    s['met'] += 1
                    meter.s.count('npcs_met')
                    log(st, 'npc met')
            return core.pad('CROSS') if k % 90 == 60 else core.NOTHING
        s['away'] = 0
        if not st.riding or st.script < menu.t + 30:
            return core.NOTHING
        if 't' not in s:
            s['t'] = st.script
            sess = st.session
            lvl = (sess >> 16) & 0xFF
            rider_id = run.mem(dm2.RIDER_ID, 1)[0]
            rec = run.mem(dm2.CAREER + rider_id * dm2.CAREER_RIDER + 0x13C + lvl * 0x44, 0x44)
            s['level_block'], s['rider_id'] = lvl, rider_id
            log(st, 'ride starts: session %08x level block %d rider %d challenge %d open %x goals done %d' % (
                sess, lvl, rider_id, rec[0x3D], rec[0x3C], dm2.goals_done(rec)))
        watch(st)
        # a game screen over the play while the rider's state still reads riding (the NPC's, 27): CROSS now and then
        screen = run.mem(sym['g_gameScreen'], 1)[0]
        if screen:
            k = s['over'] = s.get('over', 0) + 1
            if k == 1:
                s['screens'].append(screen)
                log(st, 'screen %d over the play' % screen)
                if any(n['state'] == NPC_MET for n in npcs(run, sym)):
                    s['met'] += 1
                    meter.s.count('npcs_met')
                    log(st, 'npc met')
            return core.pad('CROSS') if k % 90 == 60 else core.NOTHING
        s['over'] = 0
        t = st.script - s['t']
        # the run's time is up (ProQuest, Session): the rider stands in mid-move while the clock goes on (dm2.py's
        # test); the game over's way out from here
        moving = any(abs(v) > 0.001 for v in st.vel)
        s['same'] = s.get('same', 0) + 1 if st.pos == s.get('pos') and moving else 0
        s['pos'] = st.pos
        if s['same'] > 200 and any(x['kind'] == 'exit' for x in steps):
            log(st, 'run finished: score %.0f; exit by the game over' % st.run_score)
            meter.s.count('finished')
            s['exit'], s['ek'] = 'over', 0
            return core.NOTHING
        pl.step_frames += 1
        at_exit = pl.step_i < len(steps) and steps[pl.step_i]['kind'] == 'exit'
        if t > ticks or at_exit:
            if not any(x['kind'] == 'exit' for x in steps[pl.step_i:]):
                return None
            log(st, 'exit: END RUN, EXIT TO MAIN MENU')
            s['exit'] = 'pause'
            return core.NOTHING
        return pl(st)

    with core.Run(cfg) as run:
        box['run'] = run
        rec = core.Recorder()
        core.play(run, dm2.read, policy, max_ticks=ticks + menu.t + 40000, recorder=rec)
        if 'level_block' in s:
            try:
                r = run.mem(dm2.CAREER + s['rider_id'] * dm2.CAREER_RIDER + 0x13C + s['level_block'] * 0x44, 0x44)
                s['goals_done'] = dm2.goals_done(r)
            except Exception:
                pass
    stats = meter.s.json()
    stats.update(steps=s['results'], reached=s['reached'], npcs_met=s['met'], screens=sorted(set(s['screens'])),
                 touched=s['touched'], npc_collisions=s['collisions'], goals_done=s.get('goals_done'),
                 areas=len(pl.seen), end=run.script_time)
    core.save_json(os.path.join(d, 'stats.json'), stats)
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(menu.text() + ',' + rec.text())
    print(name, meter.s.row(dm2.COLUMNS), '| steps', s['results'], '| reached', s['reached'], '| npcs met', s['met'],
          '| npc collisions', s['collisions'])


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if '=' not in x]
    o = dict(x.split('=', 1) for x in sys.argv[1:] if '=' in x)
    if len(a) < 4:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]), a[2], a[3], int(a[4]) if len(a) > 4 else 9000, int(a[5]) if len(a) > 5 else 1,
         tier=int(o['tier']) if o.get('tier') else None, max_secs=int(os.environ.get('MAX_SECS', '2400')))
