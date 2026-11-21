#!/usr/bin/env python3
"""BMX XXX: a played run that goes after the level's goals by the game's own tables and state (routes2), on bmx.py's
player: the NPC riders met (g_npcs), goal givers' challenges taken (the .DAT's people, found live as path men), a goal's
things ridden through and its edges ground or ridden by, and at the end the run ended from the pause and the game over
screen's VIEW REPLAY watched (ZX_BMX_MENU_LINES=1).

    bmxgoalrun.py NAME LEVEL PLAN [TICKS [SEED]]

LEVEL is the level select's index (bmx.LEVELS: 0 MULY .. 7 NASA). PLAN is a comma list of steps, done in order (each
with a time limit; the player rides on between them):

    npc[:OBJECTIVE]    meet an NPC rider (the one challenging OBJECTIVE, else every one with an objective): rode up to
                       within 5 units while it stands at its replay's start, the stick back, TRIANGLE held (move 24,
                       kFlagObjectiveAsked); the NPC screen (41) answered with CROSS, which starts its challenge
    giver:GOAL         a goal giver's challenge (its .DAT person, the path man of the same objective name): beside them,
                       stopped, TRIANGLE held, the challenge's lines answered with CROSS
    collect:GOAL       through GOAL's COLLECT things in OBJCTV.TXT's order (the checkpoint last); a thing that is a
                       person (no INS instance) by the path man's live place
    edges:GOAL         GOAL's GRIND edges (EDGEn of it and its SET's children, the level map's rails of that name hash):
                       each rail taken when the map has a run-up to it, else ridden up to beside
    edges:GOAL/E/E..   (routes3) only the edges named, in that order (the ones on a floor the rider can reach)
    ride:FRAMES        the player's own riding
    pullup[:N]         (verdmbx1) the bail's pull-up: the player rides (and grinds) as it would; on a rail for 15 frames
                       it bails (the four shoulders); through every bail TRIANGLE is held (the bail's reach), and once a
                       hand holds an edge (bail sub-state 6) it stays held, the stick still, 40 frames, then the stick
                       up: the game's pull-up is physics state 11, each one counted (stats pullups). Done after N (1)
    linger:GOAL        (verdmbx1) a walker hit and stayed with: GOAL's person (its path man), ridden into until its state is
                       the after-hit one (6), then the rider kept within its touch range (PathManRecord: the drawn rider's
                       squared distance +0x80 against +0x78), creeping in when it drifts out, until the path man's own
                       count of frames a rider has been near (+0x86) reaches 1024 (the linger) or 4000 frames
    skitch[:N]         (verdmbx1) a moving car towed by: the engine's objects watched still for 300 frames, the ones
                       moving at a car's speed (0.08 .. 0.6 a frame) taken as the traffic, the rider put beside a stretch
                       of their tracks (1.8 off the line, on the map's main part) and stopped there; while a car comes
                       within 6 TRIANGLE (the skitch move) is held: the game's skitch is physics state 14 (the snap)
                       then 15 (towed), each one counted (stats skitches); held 90 frames, let go. Done after N (1)
    restart            the turn started again by the collection's pause line RESTART (after Exit Game while a turn
                       runs): the NPCs stand at their replays' starts again (they ride after their first 2700 frames)
    gameover           the run ended (pause: Exit Game, by g_menuChoices; YES), the turn's screens answered, then the
                       game over's VIEW REPLAY (by g_menuChoices) watched to its end; the run stops after it
    gameover:eyes      (routes3) as gameover, the replay camera's shot taken on (CROSS, the replay's next-shot button)
                       until it is the rider-eyes shot (11, riderViewCamera's), which is then watched
    gameover:shots     (routes3) as gameover, each of the replay camera's shots in turn, SHOT_FRAMES (120) each
    pickupat:NAME      (bmxpanel1) as pickup, the rider put at it by a poke of its place (a pickup off the ways)
    pickup:NAME        (bmxpanel1) up to a level pickup (its node's name, "Pickup_bike_frame01") and through it until the
                       game takes it (its state leaves idle: level/pickups.cpp); with GS=1 a picture and the RAM
                       (sessions/NAME/pickup-NAME-<when>.ram) as it is set out and 0, 2 and 60 frames after it is taken

Every screen change (g_gameScreen) is logged in sessions/NAME/play.log with the goal steps; stats.json has dm2.Meter's
numbers, the NPCs met, the steps' results and the replay's frames. The run's pad script is sessions/NAME/script.txt.
"""
import json
import math
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import bmx  # noqa: E402
import goalsteer  # noqa: E402

presses = bmx.presses

# The game's numbers this player goes by (games/bmx/overlay/screens.h GameScreen, screen_pause.cpp PauseChoice,
# screen_game_over.cpp GameOverChoice, screen_exit_confirm.cpp kYesLine, game/npcs.h Npc).
SCREEN_PLAY, SCREEN_REPLAY, SCREEN_PAUSE, SCREEN_EXIT_GAME, SCREEN_GAME_OVER, SCREEN_NPR = 1, 2, 7, 17, 22, 41
PAUSE_EXIT_GAME, GAME_OVER_VIEW_REPLAY, YES_LINE = 10, 5, 1
MENU_CHOICES = 16
NPCS, NPC_BYTES, NO_NPC = 3, 0x24, 0xFF
NPC_RIDER, NPC_STATES, NPC_RECORD = 0x00, 0x09, 0x18
NPC_STANDING = 0
RECORD_OBJECTIVE = 0x0C                     # NpcRecord::objectiveName (0x20)
RIDER_POSITION = 0x2650                     # the rider's position (bmx.read's pos)
MEET_RANGE = 5.0                            # meetNpc's kMeetSquared, 25
PM_TOUCH_D2, PM_DRAWN_D2 = 0x78, 0x80          # PathManRecord::touchRangeSquared, drawnRiderDistanceSquared
CAMERA_SHOT = 0x03                          # ActionCamera::shot (camera/action_camera.h)
SHOT_RIDER_EYES = 11                        # action_shot.cpp kShotRiderEyes: riderViewCamera's shot
REPLAY_FADE_FRAMES = 150                    # replayStateFrame's coming-in step (120 frames) and a little
SHOT_FRAMES = int(os.environ.get('SHOT_FRAMES', '120'))


def npcs(run, sym):
    """[{index, rider, state, sub, objective, pos}] of the level's NPCs as the game has them now."""
    data = run.mem(sym['g_npcs'], NPCS * NPC_BYTES)
    out = []
    for i in range(NPCS):
        d = data[i * NPC_BYTES:(i + 1) * NPC_BYTES]
        if d[NPC_RIDER] == NO_NPC:
            break
        rec = struct.unpack_from('<I', d, NPC_RECORD)[0]
        name = run.mem(rec + RECORD_OBJECTIVE, 0x20).split(b'\0')[0].decode('latin-1') if rec else ''
        rider = run.u32(sym['g_riders'] + 4 * d[NPC_RIDER])
        pos = struct.unpack('<3f', run.mem(rider + RIDER_POSITION, 12)) if 0x100000 <= rider < 0x2000000 else None
        out.append(dict(index=i, rider=d[NPC_RIDER], state=d[NPC_STATES], sub=d[NPC_STATES + 1], objective=name, pos=pos))
    return out


# (bmxpanel1) The level's objects (zge/scene/level_objects.h LevelObject, 0x200 records of 0x14 from levelObjectsBegin: the
# scene object +0x08, its node +0x0C, its type +0x10, whose name hash is first); a node's name at +0x128 (ZGE4);
# a pickup's state the first byte of its tag record (objectComponent: the node's tags +0x150 for an object whose scene
# (+0x38) has a type, else the object's own +0x3C; the table's records at +0x0C).
PICKUP_TYPE, PICKUP_ACTIVATING = 0x054D80A0, 2


def level_pickup(run, sym, name):
    """(scene object, its tag record) of the level pickup whose node is named `name`, or None."""
    begin = sym['levelObjectsBegin']
    data = run.mem(begin, 0x200 * 0x14)
    for i in range(0x200):
        obj, node, typ = struct.unpack_from('<3I', data, i * 0x14 + 8)
        if not (0x100000 <= obj < 0x2000000 and 0x100000 <= typ < 0x2000000 and 0x100000 <= node < 0x2000000):
            continue
        if run.u32(typ) != PICKUP_TYPE or run.mem(node + 0x128, 0x20).split(b'\0')[0].decode('latin-1') != name:
            continue
        scene = run.u32(obj + 0x38)
        table = run.u32(node + 0x150) if 0x100000 <= scene < 0x2000000 and run.mem(scene, 1)[0] else run.u32(obj + 0x3C)
        return obj, run.u32(table + 0x0C)
    return None


def live_place(run, record, fallback):
    """(routes3) Where a path man stands now (bmx.path_man_now), or `fallback` (its .DAT place) while it has no
    follower yet (PathManTravel::follower 0 until it is put on a path: the place reads as 0, 0, 0)."""
    rec, info = record
    if not info.get('follower'):
        return fallback
    pos = bmx.path_man_now(run, rec, info)[0]
    return fallback if pos == (0.0, 0.0, 0.0) and fallback is not None else pos


def short_run_up(level, pts, backs=(8, 6, 10, 5, 12, 4, 3)):
    """(routes3) A way onto a goal's edge from its end pts[0] (player_dm2.rail_entry's dict) with a run-up of `backs`
    units along its line: the floor at the run-up's start in a large part, a hop (-0.4 .. 2.1) below the edge, and
    flat (steps under 1.2) all the way to the edge's end: for the short edges on a curb that the player's run-ups
    (14 to 26 back) miss, Syracuse's edge_antenna03 and 04."""
    import math
    a, b = pts[0], pts[1]
    d = math.hypot(b[0] - a[0], b[2] - a[2])
    if d < 0.5:
        return None
    t = ((b[0] - a[0]) / d, (b[2] - a[2]) / d)
    for back in backs:
        start = (a[0] - t[0] * back, a[1], a[2] - t[1] * back)
        node = level.nearest(start, radius=2)
        if not node or level.part.get(node) is None or level.sizes[level.part[node]] < 2000:
            continue
        floor = level.world(node)
        if not (-0.4 <= floor[1] - a[1] <= 2.1) or math.hypot(floor[0] - start[0], floor[2] - start[2]) > 3:
            continue
        y, flat = floor[1], True
        for k in range(1, 9):
            f = k / 8.0
            n2 = level.nearest((floor[0] + (a[0] - floor[0]) * f, y, floor[2] + (a[2] - floor[2]) * f), radius=1)
            if not n2 or abs(level.world(n2)[1] - y) > 1.2:
                flat = False
                break
            y = level.world(n2)[1]
        if flat:
            return dict(pts=pts, length=math.dist(a, pts[-1]), rise=floor[1] - a[1], start=floor, tangent=t,
                        part=level.part.get(node))
    return None


def menu_choices(run, sym):
    return list(run.mem(sym['g_menuChoices'], MENU_CHOICES))


def goal_edges(level, goal):
    """The EDGEn names of GOAL's GRIND objectives: GOAL itself, its SET children and their children (OBJCTV.TXT)."""
    import bmxgoals
    text = open(os.path.join(bmxgoals.DISC, 'ASSETS', 'LEVELS', level, 'OBJCTV.TXT'), encoding='latin-1').read()
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    blocks = {}
    for body in re.findall(r'OBJECTIVE_BEGIN(.*?)OBJECTIVE_END', text, flags=re.S):
        m = re.search(r'NAME\s+"([^"]*)"', body)
        if m:
            blocks.setdefault(m.group(1), body)
    out, todo, seen = [], [goal], set()
    while todo:
        name = todo.pop(0)
        if name in seen or name not in blocks:
            continue
        seen.add(name)
        body = blocks[name]
        out += re.findall(r'EDGE\d+\s+"([^"]*)"', body)
        todo += re.findall(r'OBJECTIVE\d+\s+"([^"]*)"', body)
    return out


def people(level):
    """[(goal name as the .DAT person has it, model, place)]: bmxgoals.givers with the goal matched in any case (MNCO's
    lounge singer holds "lounge" for the goal Lounge)."""
    import bmxgoals
    base = os.path.join(bmxgoals.DISC, 'ASSETS', 'LEVELS', level)
    objs = bmxgoals.objectives(open(os.path.join(base, 'OBJCTV.TXT'), encoding='latin-1').read())
    goals = {o['name'].split('.')[0].lower(): o['name'].split('.')[0] for o in objs if o['name']}
    out = []
    for cls, pos, r in bmxgoals.data_points(open(os.path.join(base, level + '.DAT'), 'rb').read()):
        if cls != bmxgoals.PERSON_CLASS:
            continue
        words = [w.decode('latin-1') for w in re.findall(rb'[ -~]{4,}', r)]
        named = [(len(g), g, w) for w in words for g in goals if w.lower().endswith(g)]
        models = [w for w in words if re.match(r'.?n?[a-z]{2}[A-Z]{4,6}', w)]
        if named:
            _, g, w = max(named)
            out.append(dict(goal=goals[g], held=w[len(w) - len(g):], model=models[0] if models else None, pos=pos))
    return out


def play(name, level, plan, ticks=9000, seed=1, max_secs=2400):
    import dm2
    import bmxmap  # noqa: F401  (BMX's maps for dm2map)
    import bmxgoals
    import dm2map
    import dm2_level as dl
    import player_dm2 as pd
    sym = goalsteer.Symbols('bmx')
    level_name = bmx.LEVELS[level][0]
    menu = bmx.menu_script(level)
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    env = {'ZX_EE_SCALE': '4096', 'ZX_BMX_MENU_LINES': '1'}
    for kv in filter(None, os.environ.get('PLAY_ENV', '').split(';')):
        k, v = kv.split('=', 1)
        env[k] = v
    cfg = bmx.run_config(name, menu.text(), env=env, max_secs=max_secs, gs=os.environ.get('GS') == '1')
    meter, log = dm2.Meter(), pd.Log(os.path.join(d, 'play.log'))
    lv = dm2map.Level(level_name)
    pl = bmx.player_of(lv, seed, log)
    objs, where = bmxgoals.level_goals(level_name)
    persons = people(level_name)
    log.f.write('plan %s\npeople %s\n' % (plan, ', '.join('%s %s (%.0f,%.0f,%.0f)' % (p['goal'], p['held'], *p['pos']) for p in persons)))
    steps = [dict(kind=s.split(':', 1)[0], arg=s.split(':', 1)[1] if ':' in s else '') for s in plan.split(',') if s]
    s = dict(program=[], met=[], screens=[], results=[], replay_frames=0, game_over_visits=0)
    run_box = {}

    moments = []   # (bmxpanel1) (tick, name): a picture and the RAM to keep then

    def keep_moment(st, name, afters):
        for after in afters:
            moments.append((st.script + after, '%s+%d' % (name, after)))

    watched, seen_taken = {}, set()   # (bmxpanel1) pickups put at: their tag records, watched each frame

    def keep_moments(st):
        if os.environ.get('GS') != '1':
            return
        for name, tags in list(watched.items()):
            if run_box['run'].mem(tags, 1)[0] >= PICKUP_ACTIVATING:
                del watched[name]
                if name not in seen_taken:
                    seen_taken.add(name)
                    log(st, 'pickup %s taken' % name)
                    keep_moment(st, 'pickup-%s-taken' % name, (0, 2, 60))
        for at, name in [m for m in moments if m[0] <= st.script]:
            moments.remove((at, name))
            run = run_box['run']
            with open(os.path.join(d, name + '.ram'), 'wb') as f:
                f.write(run.mem(0, 0x2000000))
            log(st, 'shot', run.shot(name))

    class Meet(pd.Task):
        """Up to a person or rider (where(st) gives their place now), slowing on the last stretch, then stopped with
        TRIANGLE held; done when `done()` says the game took it, or after `hold` frames."""
        name = 'meet'

        def __init__(self, player, where, done, label, near=3.0, hold=360, rise=4.0):
            super().__init__(player)
            self.where, self.done, self.label, self.hold = where, done, label, hold
            self.sub = goalsteer.Approach(player, where, near=near, rise=rise, slow=0.14, limit=2400, label=label)
            self.standing = False
            self.took = False   # the game took the rider off the play (a challenge's lines): set by the policy
            self.held = 0

        def step(self, st):
            self.frames += 1
            if self.done() or self.took:
                return None
            if self.sub:
                r = self.sub.step(st)
                if r is not None:
                    return r
                if not self.sub.reached:
                    return None
                self.sub = None
                self.standing = True
            goal = self.where(st)
            if goal is None:
                return None
            self.held += 1
            dist = goalsteer.flat(st.pos, goal)
            if self.held == 1:
                self.p.log(st, 'meet %s: TRIANGLE held, %.1f away' % (self.label, dist))
            if self.held > self.hold:
                self.p.log(st, 'meet %s: not taken after %d frames, %.1f away' % (self.label, self.held, dist))
                return None
            lx, _ = pd.steer(st, goal)
            if dist > 3.5 and st.speed < 0.08:
                return core.pad('TRIANGLE', 'UP', lx=lx)     # drifted off: creep back in
            return core.pad('TRIANGLE', lx=lx, ly=1.0 if st.speed > 0.02 else None)

    class Through(pd.Task):
        """Into a person at speed (a COLLECT of a person: the game takes it when the rider's drawn body touches them):
        to within `back` of them, then straight through their place now and on past it, `passes` times."""
        name = 'through'
        limit = 2400

        def __init__(self, player, where, label, passes=3, record=None):
            super().__init__(player)
            self.where, self.label, self.passes = where, label, passes
            self.record, self.nearest_drawn = record, 1e9
            self.sub = goalsteer.Approach(player, where, near=6.0, limit=1500, label=label)
            self.run = 0
            self.done_passes = 0
            self.closest = 1e9

        def step(self, st):
            self.frames += 1
            if self.frames > self.limit:
                return None
            if self.sub:
                r = self.sub.step(st)
                if r is not None:
                    return r
                if not self.sub.reached:
                    return None
                self.sub = None
            goal = self.where(st)
            if goal is None:
                return None
            d = goalsteer.flat(st.pos, goal)
            self.closest = min(self.closest, d)
            if self.record:
                # (routes3) the path man's own view of the rider (PathManRecord: +0x80 the drawn body's squared
                # distance, against +0x78 the touch range's)
                self.nearest_drawn = min(self.nearest_drawn, struct.unpack('<f', run_box['run'].mem(self.record + PM_DRAWN_D2, 4))[0])
            self.run += 1
            if self.run > 150 or (self.run > 20 and d > 8.0):
                self.done_passes += 1
                self.p.log(st, 'through %s: pass %d, closest %.2f' % (self.label, self.done_passes, self.closest))
                if self.record:
                    r = run_box['run'].mem(self.record, 0xCC)
                    self.p.log(st, 'through %s: path man state %d paused %d, touch range^2 %.2f, nearest drawn^2 %.2f, '
                               'objective %#x' % (self.label, *struct.unpack_from('<i', r, 0x10),
                                                  *struct.unpack_from('<i', r, 0x10 + 0x54),
                                                  *struct.unpack_from('<f', r, PM_TOUCH_D2), self.nearest_drawn,
                                                  *struct.unpack_from('<I', r, 0xC8)))
                    self.nearest_drawn = 1e9
                if self.done_passes >= self.passes:
                    return None
                self.sub = goalsteer.Approach(self.p, self.where, near=6.0, limit=900, label=self.label)
                self.run, self.closest = 0, 1e9
                return core.pad('UP')
            lx, _ = pd.steer(st, goal)
            return core.pad('UP', lx=lx)

    def moving_objects(run):
        """{object address: position} of every object on the engine's list (g_objectList: next, _, object)."""
        out, node, n = {}, run.u32(sym['g_objectList']), 0
        while 0x100000 <= node < 0x2000000 and n < 20000:
            n += 1
            nxt, _, obj = struct.unpack('<3I', run.mem(node, 12))
            if 0x100000 <= obj < 0x2000000:
                out[obj] = struct.unpack('<3f', run.mem(obj + 0x10, 12))
            node = nxt
        return out

    class Skitch(pd.Task):
        """(verdmbx1) The skitch: see the step's description (skitch[:N])."""
        name = 'skitch'
        limit = 20000
        TRACK, CAR_SLOW, CAR_FAST, OFF, NEAR, HOLD = 300, 0.08, 0.6, 1.8, 6.0, 90

        def __init__(self, player, want):
            super().__init__(player)
            self.want, self.phase, self.tracks, self.last = want, 'track', {}, {}
            self.cars, self.spot, self.sub, self.held, self.skitches, self.waits = set(), None, None, 0, 0, 0
            self.spots_tried = []

        def car_states(self, run):
            now = moving_objects(run)
            states = {}
            for a, p in now.items():
                q = self.last.get(a)
                if q is not None:
                    v = (p[0] - q[0], p[1] - q[1], p[2] - q[2])
                    states[a] = (p, v, math.hypot(v[0], v[2]))
            self.last = now
            return states

        def pick_spot(self, st):
            # a car track's point nearest the rider that has a floor of the main part 1.8 to one side of it
            part = self.p.level.main_part(st.pos)
            best = None
            for a in self.cars:
                tr = self.tracks[a]
                for i in range(5, len(tr) - 5, 5):
                    p, v = tr[i]
                    sp = math.hypot(v[0], v[2])
                    if sp < 1e-3:
                        continue
                    nx, nz = -v[2] / sp, v[0] / sp
                    for side in (1, -1):
                        w = (p[0] + side * nx * self.OFF, p[1], p[2] + side * nz * self.OFF)
                        node = self.p.level.nearest(w, radius=1)
                        if node is None or self.p.level.part.get(node) != part:
                            continue
                        key = (round(w[0]), round(w[2]))
                        if key in self.spots_tried:
                            continue
                        d = goalsteer.flat(st.pos, w)
                        if best is None or d < best[0]:
                            best = (d, w, a, sp)
            return best

        @property
        def over(self):
            return self.frames > self.limit or self.skitches >= self.want or self.phase == 'none'

        @property
        def standing(self):
            return self.phase in ('track', 'wait')

        def step(self, st):
            self.frames += 1
            run = run_box['run']
            if self.over:
                self.p.log(st, 'skitch: %d skitches' % self.skitches)
                return None
            cars = self.car_states(run)
            if st.physics in (14, 15):
                if self.held == 0 or st.physics != getattr(self, 'last_physics', 0):
                    self.p.log(st, 'skitch: physics %d (%s), %d frames held' % (st.physics, 'snap' if st.physics == 14 else 'towed', self.held))
                if st.physics == 15 and getattr(self, 'last_physics', 0) != 15:
                    self.skitches += 1
                    meter.s.count('skitches')
                    self.towed = 0
                self.last_physics = st.physics
                self.towed = getattr(self, 'towed', 0) + 1
                return core.pad('TRIANGLE') if self.towed < self.HOLD else core.NOTHING
            self.last_physics = st.physics
            if self.phase == 'track':
                for a, (p, v, sp) in cars.items():
                    self.tracks.setdefault(a, []).append((p, v))
                if self.frames >= self.TRACK:
                    for a, tr in self.tracks.items():
                        sps = sorted(math.hypot(v[0], v[2]) for _, v in tr)
                        med = sps[len(sps) // 2] if sps else 0
                        if self.CAR_SLOW < med < self.CAR_FAST:
                            self.cars.add(a)
                    self.p.log(st, 'skitch: %d moving objects, %d at a car\'s speed (%s)' % (
                        len([a for a, tr in self.tracks.items() if tr and max(math.hypot(v[0], v[2]) for _, v in tr) > 0.02]),
                        len(self.cars), ', '.join('%#x %.2f' % (a, sorted(math.hypot(v[0], v[2]) for _, v in self.tracks[a])[len(self.tracks[a]) // 2]) for a in sorted(self.cars))))
                    if not self.cars:
                        self.phase = 'none'
                        return None
                    self.phase = 'spot'
                return core.NOTHING
            if self.phase == 'spot':
                best = self.pick_spot(st)
                if best is None:
                    self.p.log(st, 'skitch: no place beside the cars\' tracks')
                    self.phase = 'none'
                    return None
                self.spot = best[1]
                self.spots_tried.append((round(self.spot[0]), round(self.spot[2])))
                self.p.log(st, 'skitch: to (%.1f %.1f %.1f), beside car %#x (%.2f a frame), %.1f off' % (*self.spot, best[2], best[3], best[0]))
                self.sub = goalsteer.Approach(self.p, lambda st2, w=self.spot: w, near=1.0, limit=1500, slow=0.12, label='skitch place')
                self.phase = 'go'
            if self.phase == 'go':
                r = self.sub.step(st)
                if r is not None:
                    return r
                self.phase = 'wait' if self.sub.reached else 'spot'
                self.waits = 0
                return core.NOTHING
            # waiting, still: TRIANGLE while a car comes near
            self.waits += 1
            if self.waits > 2400:
                self.p.log(st, 'skitch: no car came by in 2400 frames')
                self.phase = 'spot'
                return core.NOTHING
            near = [(goalsteer.flat(st.pos, p), a, sp) for a, (p, v, sp) in cars.items() if a in self.cars]
            if near and min(near)[0] < self.NEAR:
                d, a, sp = min(near)
                self.held += 1
                if self.held % 30 == 1:
                    self.p.log(st, 'skitch: car %#x %.1f off (%.2f a frame), TRIANGLE held' % (a, d, sp))
                return core.pad('TRIANGLE')
            self.held = 0
            if goalsteer.flat(st.pos, self.spot) > 3.0:
                self.phase = 'go'
                self.sub = goalsteer.Approach(self.p, lambda st2, w=self.spot: w, near=1.0, limit=900, slow=0.12, label='skitch place')
            return core.NOTHING

    class Linger(pd.Task):
        """(verdmbx1) linger:GOAL's (see its description)."""
        name = 'linger'
        limit = 6000

        def __init__(self, player, where, record, label):
            super().__init__(player)
            self.where, self.record, self.label = where, record, label
            self.sub = goalsteer.Approach(player, where, near=5.0, limit=2400, label=label)
            self.phase, self.most, self.stay = 'go', 0, 0
            self.over = False

        @property
        def standing(self):
            return self.phase == 'stay'

        def step(self, st):
            self.frames += 1
            run = run_box['run']
            r = run.mem(self.record, 0x88)
            state = struct.unpack_from('<i', r, 0x10)[0]
            touch, drawn = struct.unpack_from('<f', r, PM_TOUCH_D2)[0], struct.unpack_from('<f', r, PM_DRAWN_D2)[0]
            near = struct.unpack_from('<H', r, 0x86)[0]
            self.most = max(self.most, near)
            if near >= 1024 or self.frames > self.limit:
                self.p.log(st, 'linger %s: the path man\'s near count %d (most %d), state %d' % (self.label, near, self.most, state))
                if near >= 1024:
                    meter.s.count('lingers')
                self.over = True
                return None
            goal = self.where(st)
            if goal is None:
                return core.NOTHING
            if self.phase == 'go':
                if self.sub:
                    a = self.sub.step(st)
                    if a is not None:
                        return a
                    self.sub = None
                    self.phase = 'hit'
                    self.hit = 0
                return core.NOTHING
            if self.phase == 'hit':
                self.hit += 1
                if state == 6:
                    self.p.log(st, 'linger %s: hit (path man state 6), drawn^2 %.2f, touch^2 %.2f' % (self.label, drawn, touch))
                    self.phase = 'stay'
                    return core.NOTHING
                if self.hit > 900:
                    self.p.log(st, 'linger %s: not hit (state %d)' % (self.label, state))
                    self.sub = goalsteer.Approach(self.p, self.where, near=5.0, limit=1500, label=self.label)
                    self.phase = 'go'
                    return core.NOTHING
                lx, _ = pd.steer(st, goal)
                return core.pad('UP', lx=lx)
            # stay: within the touch range, creeping in when out
            self.stay += 1
            if self.stay % 120 == 1:
                self.p.log(st, 'linger %s: state %d, drawn^2 %.2f of %.2f, near count %d' % (self.label, state, drawn, touch, near))
            lx, _ = pd.steer(st, goal)
            if drawn > touch * 0.6 and st.speed < 0.03 and self.stay % 8 == 0:
                return core.pad('UP', lx=lx)
            return core.pad(lx=lx) if st.speed < 0.01 else core.pad('DOWN')

    class GoalPlayer(type(pl)):
        step_i = 0
        step_frames = 0
        step_task = None

        def ride(self, st):
            if getattr(self.task, 'standing', False):
                self.slow = 0
            return super().ride(st)

        def next_task(self, st):
            t = self.goal_task(st)
            if t is not None:
                self.step_task = t
                self.log(st, 'goal task', t.name, getattr(t, 'label', ''))
                return t
            return super().next_task(st)

        def finish(self, st, result):
            step = steps[self.step_i]
            s['results'].append('%s:%s %s' % (step['kind'], step['arg'], result))
            self.log(st, 'step %s:%s %s' % (step['kind'], step['arg'], result))
            self.step_i += 1
            self.step_frames = 0
            self.step_task = None
            step.pop('todo', None)

        def goal_task(self, st):
            run = run_box['run']
            while self.step_i < len(steps):
                step = steps[self.step_i]
                kind, arg = step['kind'], step['arg']
                if kind == 'gameover':
                    return None
                if kind == 'restart':
                    if not s.get('r'):
                        s['r'] = 'pause'
                        self.finish(st, 'asked')
                    return None
                if kind == 'ride':
                    if self.step_frames > int(arg or '600'):
                        self.finish(st, 'ridden')
                        continue
                    return None
                if kind == 'linger':
                    t = step.get('task')
                    if t is None:
                        person = [p for p in persons if p['goal'].lower() == arg.lower()]
                        found = bmx.path_men(run, {person[0]['held'], person[0]['goal']}) if person else {}
                        if not found:
                            self.finish(st, 'no path man')
                            continue
                        k, v = min(found.items(), key=lambda kv: goalsteer.flat(bmx.path_man_now(run, kv[0], kv[1])[0], person[0]['pos']))

                        def where(st2, rec=(k, v), p=person[0]):
                            return live_place(run, rec, p['pos'])
                        t = step['task'] = Linger(self, where, k, 'linger %s' % arg)
                    if not t.over:
                        return t
                    self.finish(st, 'near count %d' % t.most)
                    continue
                if kind == 'pullup':
                    ps = step.setdefault('ps', dict(n=0))
                    if ps['n'] >= int(arg or '1') or self.step_frames > int(step.get('limit', 20000)):
                        self.finish(st, '%d pull-ups' % ps['n'])
                        continue
                    return None        # (the policy's pull-up hook acts; the player rides between)
                if kind == 'skitch':
                    t = step.get('task')
                    if t is None:
                        t = step['task'] = Skitch(self, int(arg or '1'))
                    if not t.over:
                        return t           # (again after a bail or a stop the player took it off for)
                    self.finish(st, '%d skitches' % t.skitches)
                    continue
                if self.step_frames > int(step.get('limit', 9000)):
                    self.finish(st, 'given up (time)')
                    continue
                if kind == 'npc':
                    met = [m for m in s['met'] if not arg or m == arg]
                    if met and (arg or step.get('all_met')):
                        self.finish(st, 'met %s' % ', '.join(met))
                        continue
                    here = npcs(run, sym)
                    if self.step_frames == 0 or step.get('seen') is None:
                        step['seen'] = True
                        self.log(st, 'npcs: %s' % '; '.join('%d %s state %d/%d at %s' % (
                            n['index'], n['objective'] or '-', n['state'], n['sub'],
                            '(%.0f %.0f %.0f)' % n['pos'] if n['pos'] else '?') for n in here))
                    gone = step.setdefault('gone', set())       # given up: not met in 4 tries
                    want = [n for n in here if n['objective'] and n['objective'] not in s['met'] and n['objective'] not in gone
                            and (not arg or n['objective'] == arg)]
                    if not want:
                        if not arg:
                            step['all_met'] = True
                        self.finish(st, 'met %s' % (', '.join(s['met']) or 'none'))
                        continue
                    standing = [n for n in want if n['state'] == NPC_STANDING and n['pos'] and n['pos'] != (0.0, 0.0, 0.0)]
                    if not standing:
                        return None            # riding their replays: ride on, ask again
                    n = min(standing, key=lambda n: goalsteer.flat(st.pos, n['pos']))
                    tries = step.setdefault('tries', {})
                    tries[n['objective']] = tries.get(n['objective'], 0) + 1
                    if tries[n['objective']] > 4:
                        gone.add(n['objective'])
                        self.log(st, 'npc %s: given up' % n['objective'])
                        continue
                    index, objective = n['index'], n['objective']

                    def where(st2, index=index):
                        now = [m for m in npcs(run, sym) if m['index'] == index]
                        return now[0]['pos'] if now and now[0]['state'] == NPC_STANDING else None
                    return Meet(self, where, lambda o=objective: o in s['met'], 'npc %s' % objective)
                if kind == 'giver':
                    person = [p for p in persons if p['goal'].lower() == arg.lower()]
                    if not person or step.get('tries', 0) >= 3:
                        self.finish(st, 'no giver' if not person else 'not taken')
                        continue
                    if step.get('taken') or (self.step_task is not None and getattr(self.step_task, 'took', False)):
                        self.finish(st, 'taken')
                        continue
                    step['tries'] = step.get('tries', 0) + 1
                    p = person[0]
                    if step.get('record') is None:
                        found = bmx.path_men(run, {p['held'], p['goal']})
                        best = None
                        for k, v in found.items():
                            pos, _ = bmx.path_man_now(run, k, v)
                            dd = goalsteer.flat(pos, p['pos'])
                            if best is None or dd < best[0]:
                                best = (dd, k, v)
                        step['record'] = best[1:] if best else ()
                        s.setdefault('records', {})[p['goal'].lower()] = step['record']
                        self.log(st, 'giver %s: %s' % (arg, 'path man %#x, objective %#x, %.1f from its data point' % (
                            best[1], best[2]['objective'], best[0]) if best else 'no path man of the name'))

                    def where(st2, p=p, step=step):
                        if step['record']:
                            return live_place(run, step['record'], p['pos'])
                        return p['pos']

                    def taken(step=step):
                        rec = step['record']
                        if rec and rec[1]['objective'] and bmx.objective_state(run, rec[1]['objective']) >= bmx.OBJECTIVE_ACTIVE:
                            step['taken'] = True
                        return step.get('taken', False)
                    return Meet(self, where, taken, 'giver %s' % arg, near=2.2)
                if kind in ('pickup', 'pickupat'):
                    found = level_pickup(run, sym, arg)
                    if found is None or step.get('tries', 0) >= 4:
                        self.finish(st, 'no pickup of the name' if found is None else 'not taken')
                        continue
                    obj, tags = found
                    if run.mem(tags, 1)[0] >= PICKUP_ACTIVATING:
                        if arg not in seen_taken:
                            seen_taken.add(arg)
                            keep_moment(st, 'pickup-%s-taken' % arg, (0, 2, 60))
                        self.finish(st, 'taken')
                        continue
                    if not step.get('tries'):
                        keep_moment(st, 'pickup-%s-before' % arg, (0,))
                    step['tries'] = step.get('tries', 0) + 1

                    def where(st2, obj=obj):
                        return struct.unpack('<3f', run.mem(obj + 0x10, 12))
                    if kind == 'pickupat':
                        # (a pickup the player's ways do not reach: the rider put at it, poked, for the game to take it)
                        for k, v in enumerate(where(st)):
                            run.command('poke %#x %d' % (st.rider + RIDER_POSITION + 4 * k, struct.unpack('<I', struct.pack('<f', v))[0]))
                        self.log(st, 'pickupat %s: the rider put at (%.1f %.1f %.1f)' % ((arg,) + where(st)))
                        step['tries'] = step.get('tries', 0) + 1
                        watched[arg] = tags
                        return None
                    return goalsteer.Approach(self, where, near=0.6, limit=2400, label='pickup %s' % arg)
                if kind == 'collect':
                    todo = step.get('todo')
                    if todo is None:
                        todo = []
                        for o in sorted((o for o in objs if o['type'] == 'COLLECT' and o['name'] and o['name'].split('.')[0].lower() == arg.lower()),
                                        key=lambda o: 'chkpt' in o['name'].lower()):
                            for item, w in zip(o['items'], o['where']):
                                todo.append((item, w))
                        step['todo'] = todo
                        self.log(st, 'collect %s: %s' % (arg, ', '.join(i for i, _ in todo)))
                    if not todo:
                        self.finish(st, 'done')
                        continue
                    if os.environ.get('COLLECT_NEAREST') == '1':
                        # (verdmbx1) the nearest thing next (a timed goal's), the checkpoint still last
                        k = [i for i, (it, ww) in enumerate(todo) if ww is not None and not callable(ww) and 'chkpt' not in it.lower()
                             and 'checkpoint' not in it.lower()]
                        if k:
                            j = min(k, key=lambda i: goalsteer.flat(st.pos, todo[i][1]))
                            todo.insert(0, todo.pop(j))
                    item, w = todo.pop(0)
                    if w is None:
                        # a person: the giver step's path man when it found one, else the .DAT's place
                        person = [p for p in persons if (p['model'] or '').endswith(item)]
                        record = s.get('records', {}).get(person[0]['goal'].lower()) if person else None
                        if record:
                            def w(st2, record=record):
                                return live_place(run, record, None)
                        else:
                            w = person[0]['pos'] if person else None
                    if w is None:
                        self.log(st, 'collect %s: %s has no place' % (arg, item))
                        continue
                    target = w if callable(w) else (lambda st2, w=w: w)
                    if callable(w):
                        return Through(self, target, '%s %s' % (arg, item), passes=int(os.environ.get('THROUGH_PASSES', '3')), record=record[0] if record else None)
                    return goalsteer.Approach(self, target, near=1.2, limit=1800, label='%s %s' % (arg, item))
                if kind == 'edges':
                    todo = step.get('todo')
                    if todo is None:
                        # (routes3) edges:GOAL/EDGE/EDGE...: only those of GOAL's edges, in that order
                        arg, *only = arg.split('/')
                        names = goal_edges(level_name, arg)
                        if only:
                            names = [n for n in only if n in names]
                        tags = {dl.name_hash(n): n for n in names}
                        todo = []
                        for tag, n in tags.items():
                            ways = [r for r in self.rails if r.get('tag') == tag]
                            rails = [r for r in self.level.rails if r['name'] == tag]
                            if not ways:
                                # (routes3) a short run-up for a goal's edge the player's rail ways leave out
                                ways = [w for r in rails for pts in (r['points'], r['points'][::-1])
                                        for w in [short_run_up(self.level, pts)] if w]
                            todo.append(dict(edge=n, ways=ways, rails=rails))
                        step['todo'] = todo
                        self.log(st, 'edges %s: %s' % (arg, '; '.join('%s %d ways %d rails' % (e['edge'], len(e['ways']), len(e['rails'])) for e in todo)))
                    if not todo:
                        self.finish(st, 'done')
                        continue
                    e = todo.pop(0)
                    if e['ways']:
                        way = min(e['ways'], key=lambda r: goalsteer.flat(st.pos, r['start']))
                        if goalsteer.flat(st.pos, way['start']) > 12.0 and e.get('approached', 0) < 2:
                            # (routes3) far off: to the run-up's start by goalsteer's way first (the rail task's own
                            # GoTo ends at once when the paths do not lead there), then this edge again
                            e['approached'] = e.get('approached', 0) + 1
                            todo.insert(0, e)
                            self.log(st, 'edges %s: to the run-up of %s at (%.0f %.0f %.0f)' % (arg, e['edge'], *way['start']))
                            return goalsteer.Approach(self, lambda st2, g=way['start']: g, near=4.0, limit=2400,
                                                      label='run-up %s' % e['edge'])
                        self.log(st, 'edges %s: rail %s' % (arg, e['edge']))
                        return pd.Rail(self, way)
                    if not e['rails']:
                        continue
                    pts = e['rails'][0]['points']
                    mid = pts[len(pts) // 2]
                    # the floor nearest the edge (across) in the part of the level the rider is in: no drop into a pit
                    part = self.level.main_part(st.pos)
                    floors = [n for n, k in self.level.part.items() if k == part]
                    node = min(floors, key=lambda n: goalsteer.flat(self.level.world(n), mid)) if floors else None
                    goal = self.level.world(node) if node else mid
                    self.log(st, 'edges %s: by %s, the floor (%.0f %.0f %.0f), %.1f across from the edge at y %.1f' % (
                        arg, e['edge'], *goal, goalsteer.flat(goal, mid), mid[1]))
                    return goalsteer.Approach(self, lambda st2, g=goal: g, near=4.0, limit=1500, label='edge %s' % e['edge'])
                self.finish(st, 'unknown step')
            return None

    pl.__class__ = GoalPlayer

    def over_step(st, screen):
        """The game over's way: Exit Game from the pause, YES, the turn's screens, VIEW REPLAY, its end."""
        g = s.setdefault('g', 'pause')
        k = s['gk'] = s.get('gk', 0) + 1
        if k > 12000:
            log(st, 'gameover: not done (%s)' % g)
            return None
        if g == 'pause':
            if st.mode == bmx_paused and screen == SCREEN_PAUSE:
                s['g'] = 'choose'
                # (the menu's Init takes its time: a DOWN in it is lost; the wait longer at each try)
                s['program'] = presses(120 * s.get('tries', 1))
                return s['program'].pop(0)
            return core.pad('START') if k % 120 < 6 else core.NOTHING
        if g == 'choose':
            # (the menu's lines are made as the screen comes in: read again until Exit Game is among them)
            choices = menu_choices(run, sym)
            if PAUSE_EXIT_GAME not in choices:
                s['choose_k'] = s.get('choose_k', 0) + 1
                if s['choose_k'] > 600:
                    log(st, 'pause choices %s: no Exit Game' % choices)
                    return None
                return core.NOTHING
            log(st, 'pause choices %s (after %d frames)' % (choices, s.get('choose_k', 0)))
            s['g'] = 'confirm'
            s['program'] = presses(*['DOWN'] * choices.index(PAUSE_EXIT_GAME), 'CROSS', 90)
            return s['program'].pop(0)
        if g == 'confirm':
            if screen == SCREEN_EXIT_GAME:
                s['g'] = 'over'
                s['program'] = presses(60, *['DOWN'] * YES_LINE, 'CROSS', 150)
                return s['program'].pop(0)
            if screen not in (SCREEN_PAUSE, SCREEN_EXIT_GAME) and st.script - s.get('screen_at', 0) > 60:
                # another line's screen: back to the pause, play again, and a new try with a longer wait
                s['tries'] = s.get('tries', 1) + 1
                log(st, 'gameover: screen %d instead of the exit, try %d' % (screen, s['tries']))
                if s['tries'] > 3:
                    return None
                s['g'], s['gk'] = 'back', 0
                return core.pad('TRIANGLE')
            return core.NOTHING
        if g == 'back':
            if screen == SCREEN_PAUSE and k > 90:
                s['g'], s['gk'] = 'unpause', 0
                return core.pad('START')
            return core.pad('TRIANGLE') if k % 90 < 6 else core.NOTHING
        if g == 'unpause':
            if st.mode != bmx_paused and screen == SCREEN_PLAY and k > 120:
                s['g'], s['gk'] = 'pause', 0
            return core.NOTHING
        if g == 'over':
            if screen == SCREEN_GAME_OVER:
                choices = menu_choices(run, sym)
                if GAME_OVER_VIEW_REPLAY not in choices:
                    s['over_k'] = s.get('over_k', 0) + 1
                    if s['over_k'] > 600:
                        log(st, 'game over choices %s: no VIEW REPLAY' % choices)
                        return None
                    return core.NOTHING
                s['game_over_visits'] += 1
                log(st, 'game over choices %s (after %d frames)' % (choices, s.get('over_k', 0)))
                s['g'] = 'replay'
                s['program'] = presses(90, *['DOWN'] * choices.index(GAME_OVER_VIEW_REPLAY), 'CROSS', 30)
                return s['program'].pop(0)
            # the turn's summaries (records, rewards, the goals' list): CONTINUE once each has been up a while
            if st.script - s.get('screen_at', 0) > 90 and k % 120 < 4:
                return core.pad('CROSS')
            return core.NOTHING
        if g == 'replay':
            if screen == SCREEN_REPLAY:
                s['replay_frames'] += 1
                if shots_wanted:
                    return shot_step(st)
                return core.NOTHING
            if s['replay_frames'] > 30 and screen == SCREEN_GAME_OVER:
                s['g'] = 'done'
                log(st, 'replay over: %d frames' % s['replay_frames'])
                s['program'] = presses(120)
                return s['program'].pop(0)
            return core.NOTHING
        return None

    bmx_paused = 3

    # (routes3) gameover:eyes: in the replay, Cross takes the replay camera's next action shot (replayStateFrame's
    # running step); pressed until the camera is on the rider-eyes shot (11), then that shot watched. gameover:shots:
    # every shot in turn, SHOT_FRAMES each.
    over_arg = next((x['arg'] for x in steps if x['kind'] == 'gameover'), '')
    shots_wanted = over_arg in ('eyes', 'shots')

    def replay_shot():
        camera = run.u32(sym['g_playerCameras'])
        return run.mem(camera + CAMERA_SHOT, 1)[0] if camera else None

    def shot_step(st):
        if s['replay_frames'] < REPLAY_FADE_FRAMES:
            return core.NOTHING
        shot = replay_shot()
        if shot != s.get('shot'):
            s['shot'], s['shot_at'] = shot, s['replay_frames']
            s.setdefault('shots', []).append(shot)
            log(st, 'replay shot %s at replay frame %d' % (shot, s['replay_frames']))
        held = s['replay_frames'] - s['shot_at']
        if over_arg == 'eyes':
            if shot == SHOT_RIDER_EYES:
                return core.NOTHING
            return core.pad('CROSS') if held > 20 and held % 30 < 4 else core.NOTHING
        return core.pad('CROSS') if held > SHOT_FRAMES and held % 30 < 4 else core.NOTHING

    def restart_step(st, screen):
        """The collection's RESTART (the pause's line after Exit Game while a turn runs): the turn again from its start,
        the NPCs waiting at their replays' starts again."""
        k = s['rk'] = s.get('rk', 0) + 1
        if s['r'] == 'pause':
            if st.mode == bmx_paused and screen == SCREEN_PAUSE and PAUSE_EXIT_GAME in menu_choices(run, sym):
                choices = menu_choices(run, sym)
                log(st, 'restart: pause choices %s' % choices)
                s['r'] = 'wait'
                s['rk'] = 0
                s['program'] = presses(120, *['DOWN'] * (choices.index(PAUSE_EXIT_GAME) + 1), 'CROSS', 60)
                return s['program'].pop(0)
            return core.pad('START') if k % 120 < 6 else core.NOTHING
        if s['r'] == 'wait':
            if screen == SCREEN_PLAY and st.mode != bmx_paused and k > 120:
                s['r'] = None
                s['restarts'] = s.get('restarts', 0) + 1
                log(st, 'restart: the turn again')
                return core.NOTHING
            if k > 1200:
                s['r'] = None
                log(st, 'restart: not back in play')
            return core.pad('CROSS') if screen not in (SCREEN_PLAY, SCREEN_PAUSE) and k % 90 == 60 else core.NOTHING
        return core.NOTHING

    def pullup_hook(st, ps):
        """(verdmbx1) pullup's acts (see its description); None: the player's own riding."""
        sub = st.actions[1]
        key = (st.physics, st.action, sub)
        if key != ps.get('key'):
            ps['key'] = key
            if st.physics in (bmx.BAIL, 11) or st.action == 8:
                log(st, 'pullup: physics %d action %d sub-state %d' % key)
        if st.physics == 11:
            if not ps.get('up'):
                ps['up'] = True
                ps['n'] += 1
                meter.s.count('pullups')
                log(st, 'pullup: physics 11, the pull-up (%d)' % ps['n'])
            return core.pad('TRIANGLE')
        ps['up'] = False
        if st.physics == bmx.GRIND:
            ps['grind'] = ps.get('grind', 0) + 1
            if ps['grind'] > 15:
                return core.pad('L1', 'R1', 'L2', 'R2', 'TRIANGLE')
            return None
        ps['grind'] = 0
        if st.action == 8 or st.physics == bmx.BAIL:
            if sub == 6:
                ps['hands'] = ps.get('hands', 0) + 1
                if ps['hands'] == 1:
                    meter.s.count('edges_held')
                return core.pad('TRIANGLE', ly=-1.0) if ps['hands'] > 40 else core.pad('TRIANGLE')
            ps['hands'] = 0
            return core.pad('TRIANGLE')
        return None

    def policy(st):
        meter.tick(st)
        keep_moments(st)
        screen = st.screen if st.frame else -1
        if (screen, st.mode) != s.get('screen'):
            s['screen'] = (screen, st.mode)
            s['screen_at'] = st.script
            s['screens'].append(screen)
            log(st, 'screen %d mode %d riding %d' % (screen, st.mode, st.riding))
            if screen == SCREEN_NPR:
                shown = run.mem(sym['g_shownNpc'], 1)[0]
                here = [n for n in npcs(run, sym) if n['index'] == shown]
                if here and here[0]['objective']:
                    s['met'].append(here[0]['objective'])
                    log(st, 'npc met: %s' % here[0]['objective'])
                    meter.s.count('npcs_met')
        if s['program']:
            return s['program'].pop(0)
        if s.get('g') == 'done':
            return None
        if s.get('g'):
            return over_step(st, screen)
        if s.get('r'):
            return restart_step(st, screen)
        if isinstance(pl.task, Meet) and pl.task.standing and (not st.riding or screen not in (SCREEN_PLAY, -1)):
            pl.task.took = True
        if st.frame and s.get('t') is not None and screen not in (SCREEN_PLAY, -1):
            # a screen over the play (the NPC's, a challenge's lines and cut scene, a reward): CROSS once it has been up
            if st.script - s['screen_at'] > 60 and (st.script - s['screen_at']) % 90 < 4:
                return core.pad('CROSS')
            return core.NOTHING
        if not st.riding:
            if s.get('t') is not None:
                return core.pad('CROSS') if st.script % 60 < 6 else core.NOTHING
            return core.NOTHING
        if st.pos[0] == 0.0 and st.pos[2] == 0.0:
            return core.pad('CROSS') if st.script % 40 < 6 else core.NOTHING
        s.setdefault('t', st.script)
        t = st.script - s['t']
        if t < bmx.DIALOG_TICKS:
            return core.pad('CROSS') if 40 <= t < 46 else core.NOTHING
        # held in place while the level's frame goes on (a challenge's cut scene and its lines, as bmx.py's Giver
        # found): CROSS shows the next line, until the rider rolls again
        holding = (isinstance(pl.task, Meet) and pl.task.standing and not pl.task.took) or isinstance(pl.task, (Skitch, Linger)) or (
            pl.step_i < len(steps) and steps[pl.step_i]['kind'] == 'pullup' and (st.action == 8 or st.physics == 11))     # (stopped on purpose)
        s['frozen'] = s.get('frozen', 0) + 1 if st.pos == s.get('last_pos') and not holding else 0
        s['last_pos'] = st.pos
        if s['frozen'] > 60:
            if s['frozen'] == 61:
                log(st, 'held in place: CROSS until the rider rolls')
                meter.s.count('held')
            if s['frozen'] > 3000:
                s['frozen'] = 0
            return core.pad('CROSS') if s['frozen'] % 90 < 4 else core.NOTHING
        pl.step_frames += 1
        if pl.step_i < len(steps) and steps[pl.step_i]['kind'] == 'pullup':
            r = pullup_hook(st, steps[pl.step_i].setdefault('ps', dict(n=0)))
            if r is not None:
                return r
        at_end = pl.step_i < len(steps) and steps[pl.step_i]['kind'] == 'gameover'
        if t > ticks or at_end:
            if not any(x['kind'] == 'gameover' for x in steps[pl.step_i:]):
                return None
            log(st, 'end run: to the game over')
            s['g'] = 'pause'
            return over_step(st, screen)
        return pl(st)

    with core.Run(cfg) as run:
        run_box['run'] = run
        pl.run = run
        rec = core.Recorder()
        core.play(run, bmx.read, policy, max_ticks=ticks + menu.t + 30000, recorder=rec)
    stats = meter.s.json()
    stats.update(npcs_met=s['met'], restarts=s.get('restarts', 0), steps=s['results'], replay_frames=s['replay_frames'],
                 game_over_visits=s['game_over_visits'], replay_shots=s.get('shots', []), areas=len(pl.seen), screens=sorted(set(s['screens'])),
                 end=run.script_time)
    core.save_json(os.path.join(d, 'stats.json'), stats)
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(menu.text() + ',' + rec.text())
    print(name, meter.s.row(dm2.COLUMNS), '| met', s['met'], '| steps', s['results'], '| replay frames', s['replay_frames'],
          '| screens', sorted(set(s['screens'])))
    print(json.dumps({k: stats.get(k) for k in ('distance', 'jumps', 'grinds', 'bails', 'score', 'best_line')}))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) < 3:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]), a[2], int(a[3]) if len(a) > 3 else 9000, int(a[4]) if len(a) > 4 else 1,
         max_secs=int(os.environ.get('MAX_SECS', '2400')))
