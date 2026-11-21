#!/usr/bin/env python3
"""Dave Mirra BMX 2: the player's table (RAM names and addresses) and how to play.

    dm2.py measure ROUTE ...   what an old route does, by the game's own numbers (no input added)
    dm2.py play NAME [mode=proquest|session|freeride|competition|mp-KIND] [level=N] [ticks=N] [seed=N] [gs=1]
    dm2.py replay NAME ...     a session's script alone, readable off: the same trace tick for tick?
    dm2.py reload NAME SAVES [PROFILE]   a native save (SAVES: its folder) loaded by OPTIONS, LOAD GAME: the career
                                         before and after (riders' open levels and challenges done)
    dm2.py watch NAME SAVES [CARD]       a native replay watched by COOL STUFF, VIEW REPLAYS (the list's first slot):
                                         the list and g_loadedReplay against Replays/Replay 01.sav (CARD: slot 1's card)
"""
import dataclasses
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402

DM2_TOOLS = zpaths.DM2_TOOLS   # (games/dm2/tools: dm2routes.py)
sys.path.insert(0, DM2_TOOLS)

SCRATCH = os.environ.get('PLAYDM2', os.path.join(zpaths.ZAXIS_SCRATCH, 'playdm2'))
RUNNER = os.environ.get('RUNNER', SCRATCH + '/bin/runner-play')
ELF = os.environ.get('ELF', os.path.join(zpaths.DM2_WORKSPACE, 'recomp', 'disc-root', 'SLUS_201.59'))

# Globals (tools/PCSX2-DM2.md, game.c).
MODE = 0x259770          # 0 boot and movies, 1 front end, 2 loading and level, 3 paused
LEVEL_FRAME = 0x25BCC4   # rider task frames since the level started; 0 in the front end
def _dm2_symbol(name):
    # a DM2 data symbol's address (tools/readable/readable_symbols.tsv)
    with open(zpaths.tree_path(zpaths.ZAXIS, 'tools', 'readable', 'readable_symbols.tsv')) as f:
        for line in f:
            w = line.rstrip('\n').split('\t')
            if len(w) >= 5 and w[0] == 'data' and w[1] == name and w[4] == 'dm2':
                return int(w[2], 16)
    raise KeyError(name)


SESSION = 0x2566C8       # bytes: +1 game type (0 ProQuest, 2 Session, 3 Free Ride, 5 Multiplayer), +2 level, +3 players
RIDER_TABLE = 0x2F4EA0   # rider objects; [0] player 1
CAREER = 0x2174B4        # 21 riders' records, 0x3C8 each; a level's at +0x13C + level x 0x44: challenges, +0x3C open, +0x3D tier
CAREER_RIDER = 0x3C8
RIDER_ID = 0x21C41F      # player 1's rider (+ player x 0x11)
CHOICE_WORD = 0x21C41C   # player 1's bike (low byte) and rider (top byte)
CHALLENGES_PER_TIER = 0x2566D0   # bytes by tier
CHALLENGES = 0x233900    # + level x 0x1B80 + tier x 0x580 + challenge x 0x160

LOADED_REPLAY = _dm2_symbol('g_loadedReplay')   # the replay VIEW REPLAYS loads and the game shows (0x2000 bytes)

# The rider (games/dm2/rider/rider.h, game.c's run counters).
# The physics states; the game's own trick names and HUD say what 7 .. 10 are: 8 a lip trick or wall ride (tricks 0x3F .. 0x45),
# 10 the manual (trick 0x46), 9 the nose wheelie (0x47), with the HUD's balance bar.
GROUND, AIR, BAIL, GRIND_APPROACH, GRIND, STALL_APPROACH, STALL, LIP_APPROACH, LIP, NOSE_WHEELIE, MANUAL = range(11)


@dataclasses.dataclass
class State:
    tick: int = 0
    script: int = 0
    mode: int = 0
    frame: int = 0
    session: int = 0
    rider: int = 0
    flags: int = 0
    action: int = 0              # the action state levels: level 0 (ActionState), sub-state, phase, ...
    actions: tuple = (0, 0, 0, 0)
    action_frames: int = 0
    trick: int = 0xFF
    bail_kind: int = 0
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
    manual_time: float = 0.0
    line_tricks: int = 0
    line_state: int = 0
    counts: tuple = (0, 0, 0, 0, 0, 0, 0)   # +0xF66: tricks, modified, modified tricks, grinds, stalls/lips, manuals, bails
    digest: bytes = None

    @property
    def riding(self):
        return self.frame > 0 and self.mode == 2 and self.rider != 0

    @property
    def game_type(self):
        return self.session >> 8 & 0xFF

    @property
    def level(self):
        return self.session >> 16 & 0xFF


def read(run):
    m = run.mem
    head = m(MODE, 4) + m(LEVEL_FRAME, 4) + m(SESSION, 4) + m(RIDER_TABLE, 4)
    mode, frame, session, rider = struct.unpack('<4I', head)
    s = State(tick=run.tick, script=run.script_time, mode=mode, frame=frame, session=session)
    s.digest = head
    if frame and 0x100000 <= rider < 0x2000000:
        s.rider = rider
        a = m(rider + 0x160, 0x20)
        b = m(rider + 0x1A0, 0x10)      # run score, _, line score, line's grind time
        c = m(rider + 0x83C, 2)
        e = m(rider + 0xF66, 14)
        f = m(rider + 0xFF0, 2)
        g = m(rider + 0x1010, 0xD0)
        s.flags = struct.unpack_from('<I', a, 0)[0]
        s.action = a[5]
        s.actions = (a[5], a[6], a[7], a[8])
        s.action_frames = struct.unpack_from('<H', a, 0xA)[0]
        s.trick = a[0x12]
        s.bail_kind = a[0x18]
        s.run_score, _, s.line_score, s.grind_time = struct.unpack('<ffIf', b)
        s.line_state, s.line_tricks = c[0], c[1]
        s.counts = struct.unpack('<7H', e)
        s.physics = f[0]
        s.up = struct.unpack_from('<3f', g, 0)
        s.fwd = struct.unpack_from('<3f', g, 0x10)
        s.pos = struct.unpack_from('<3f', g, 0x30)
        s.balance = struct.unpack_from('<f', g, 0x54)[0]
        s.vel = struct.unpack_from('<3f', g, 0x90)
        s.speed = struct.unpack_from('<f', g, 0xC8)[0]
        s.digest += a + b + c + e + f + g
    return s


class Meter:
    """What a run did, by the game's numbers, from the state of every tick."""

    def __init__(self):
        self.s = core.Stats()
        self.prev = None
        self.last_pos = None
        self.top_line = 0
        self.tricks = set()

    def tick(self, st):
        s, p = self.s, self.prev
        if not st.riding:
            s.count('menu_ticks')
            self.prev = None
            self.last_pos = None
            return
        s.count('ride_ticks')
        if st.mode == 3:
            s.count('paused_ticks')
        if self.last_pos:
            d = sum((a - b) ** 2 for a, b in zip(st.pos, self.last_pos)) ** 0.5
            if d < 3.0:        # a respawn is no ride
                s.add('distance', d)
                if st.physics == GRIND:
                    s.add('rail', d)
        self.last_pos = st.pos
        s.best('top_speed', st.speed)
        if st.physics == AIR:
            s.count('air_ticks')
        if p:
            if st.physics != p.physics:
                k = {AIR: 'jumps', BAIL: 'bails', GRIND: 'grinds', STALL: 'stalls', LIP: 'lips', NOSE_WHEELIE: 'walls', MANUAL: 'walls'}
                if st.physics in k:
                    s.count(k[st.physics])
            if st.trick != p.trick and st.trick != 0xFF:
                s.count('tricks_started')
                self.tricks.add(st.trick)
            if p.line_tricks and st.line_tricks == 0 and st.run_score > p.run_score:
                s.count('lines_banked')
                s.best('best_line_tricks', p.line_tricks)
                s.best('best_line', st.run_score - p.run_score)
            if p.line_tricks and st.line_tricks == 0 and st.run_score <= p.run_score:
                s.count('lines_lost')
        if st.physics in (NOSE_WHEELIE, MANUAL):
            s.count('manual_ticks')
        s.best('score', st.run_score)
        for name, v in zip(('c_tricks', 'c_mod', 'c_modtricks', 'c_grinds', 'c_stalls', 'c_manuals', 'c_bails'), st.counts):
            s.best(name, v)
        self.prev = st


COLUMNS = ('menu_ticks', 'ride_ticks', 'distance', 'top_speed', 'air_ticks', 'jumps', 'c_tricks', 'c_bails', 'bails', 'grinds', 'c_grinds',
           'rail', 'walls', 'c_manuals', 'stalls', 'lips', 'lines_banked', 'best_line_tricks', 'best_line', 'score', 'goals')


def level_record(run, st):
    """The 0x44 bytes of the career for this rider and level: 5 tiers of 4 challenges, 3 bytes each; +0x3C open, +0x3D tier."""
    rider = run.mem(RIDER_ID, 1)[0]
    return run.mem(CAREER + rider * CAREER_RIDER + 0x13C + st.level * 0x44, 0x44)


def challenge_targets(run, st):
    """What the open challenges of this tier name: edge and face tags (condition records 0x50 each from +0x20, the kind first).
    Kinds 8 .. 13, 23: one tag at +4; 14 .. 21: two at +4 and +8. Score kinds (22) need no place."""
    record = level_record(run, st)
    tier = record[0x3D]
    count = run.mem(CHALLENGES_PER_TIER + tier, 1)[0] if tier < 5 else 0
    out = set()
    for c in range(count):
        if record[tier * 12 + c * 3] & 0x40:
            continue
        rec = run.mem(CHALLENGES + st.level * 0x1B80 + tier * 0x580 + c * 0x160, 0x160)
        for i in range(min(rec[0], 4)):
            kind, a, b = struct.unpack_from('<3I', rec, 0x20 + i * 0x50)
            if 8 <= kind <= 13 or kind == 23:
                out.add(a)
            elif 14 <= kind <= 21:
                out.update((a, b))
    return out


def career_summary(run):
    """The whole career as the game holds it: per rider, the levels open (+0x3C) and challenges done, and their sums."""
    data = run.mem(CAREER, 21 * CAREER_RIDER)
    riders = {}
    for r in range(21):
        recs = [data[r * CAREER_RIDER + 0x13C + l * 0x44:r * CAREER_RIDER + 0x13C + (l + 1) * 0x44] for l in range(8)]
        done = [goals_done(rec) for rec in recs]
        opened = [rec[0x3C] for rec in recs]
        if any(done) or any(opened):
            riders[r] = dict(goals=done, open=opened)
    return dict(goals=sum(sum(v['goals']) for v in riders.values()), riders=riders,
                digest=__import__('hashlib').sha1(data).hexdigest()[:12])


def goals_done(record):
    return sum(1 for t in range(5) for c in range(4) if record[t * 12 + c * 3] & 0x40)


CLOCK = 4096


def run_config(name, script, env=None, max_secs=900, gs=False, root=None):
    # The gate's clock: no frame overruns its vsync, so a played run is the same run for any runner and any spec.
    e = {'ZX_DEV_UNLOCK_ALL': '1', 'ZX_READABLE': 'off', 'ZX_SAVES': 'card', 'ZX_MC_FD_BASE0': '1', 'ZX_MC_FIXED_TIME': '1',
         'ZX_EE_SCALE': str(CLOCK)}
    if os.environ.get('PLAY_FNPROF'):     # the functions a run calls (runtime ee/profile.cpp): calls per function, CSV
        os.makedirs(os.path.join(SCRATCH, 'prof'), exist_ok=True)
        e['ZX_FNPROF'] = os.path.join(SCRATCH, 'prof', name + '.csv')
    e.update(env or {})
    return core.RunConfig(name=name, runner=RUNNER, elf=ELF, root=root or SCRATCH + '/runs', script=script, env=e,
                          max_secs=max_secs, gs=gs)


def measure(route):
    import dm2routes
    script = dm2routes.script_of(route)
    end = max(int(i.split('v')[0]) for i in script.split(',')) + 600
    cfg = run_config('m-' + route, script, dm2routes.extra_env(route), max_secs=900)
    meter = Meter()
    ends = {}

    def policy(st):
        meter.tick(st)
        ends['st'] = st
        if st.riding and st.script % 300 == 0:
            meter.s.best('goals', goals_done(level_record(ends['run'], st)))
        return None if st.script >= end else core.NOTHING

    with core.Run(cfg) as run:
        ends['run'] = run
        core.play(run, read, policy, max_ticks=end + 60000)
    row = dict(meter.s.json(), trick_ids=sorted(meter.tricks), route=route, end_script=ends['st'].script, session='%08x' % ends['st'].session)
    with open(SCRATCH + '/measure.jsonl', 'a') as f:
        f.write(json.dumps(row) + '\n')
    print(route, meter.s.row(COLUMNS))


MODES = {'proquest': 'pq', 'session': 'se', 'freeride': 'fr'}
# The Xbox release's levels by their level select value (dave_mirra_xboxlevel.cpp: after Airport, the park editor's 8
# between); their maps built by dm2map.py from the bundled folder (DM2_DISC=a folder with ASSETS/LEVELS/<LEVEL>)
XBOX_LEVELS = {9: 'VENICE', 10: 'GREENVIL'}


MENU_GAP = 400     # vsyncs between presses on the rider and bike screens: a RIGHT while the last one's choice still loads
                   # leaves the screen drawing but no longer reading the pad, at any clock (rider 3, bike 2 at 30 vsyncs apart)


def menu_script(mode, level, rider=0, bike=0, modifier=0):
    """The menus' way to the level, as the routes press them: dm2routes' to_level, rider and bike by the menu at MENU_GAP;
    ProQuest and Session's two screens."""
    import dm2routes
    s = dm2routes.Script()
    if mode.startswith('mp-'):
        # MULTIPLAYER (both pads: port 1 mirrors port 0), the game type by its name (dm2routes.GAME_TYPES): the riders take
        # turns on one screen, the rider table's first entry is the one riding.
        return dm2routes.multiplayer_menu(dm2routes.GAME_TYPES.index(mode[3:]), level, s)
    plain = s.presses
    s.presses = lambda *b, after=MENU_GAP: plain(*b, after=after)
    if mode == 'competition':
        # (routes3) ProQuest's way with RUN TYPE's second line, COMPETITION (shown once the level's competition is open:
        # COMP_POKE's test-only write), before the level loads
        sync = dm2routes.Script.sync
        dm2routes.Script.sync = lambda self, cond: (self.wait(300), self.press('DOWN', after=60), self.press('CROSS', after=400),
                                                    sync(self, cond))
        try:
            dm2routes.to_level(s, 'proquest', level, rider, bike, modifier)
        finally:
            dm2routes.Script.sync = sync
        s.wait(134)
        s.press('CROSS', after=200)
        s.press('CROSS', after=200)
        s.press('CROSS', after=200)
        return s
    dm2routes.to_level(s, mode, level, rider, bike, modifier)
    if mode != 'freeride':
        s.wait(134)
        s.press('CROSS', after=200)
        s.press('CROSS', after=200)
        if mode == 'proquest':
            s.press('CROSS', after=200)
    return s


def session_dir(name):
    return os.path.join(SCRATCH, 'sessions', name)


def play_session(name, mode, level, rider=0, bike=0, seed=1, ticks=10800, gs=False, shots=(), max_secs=1500):
    """One played run: ticks of riding from the level's start. Writes sessions/NAME: script.txt (the replay), trace.txt,
    stats.json, play.log, meta.json."""
    import dm2map
    import player_dm2
    menu = menu_script(mode, level, rider, bike)
    d = session_dir(name)
    os.makedirs(d, exist_ok=True)
    multiplayer = mode.startswith('mp-')
    poke = {'ZX_PAD_PORT1': 'mirror'} if multiplayer else {}
    if mode == 'competition':
        # (routes3, test only) the level's progress flags for the rider's character: open (1) and competition open (2), a
        # word at CAREER + character x 0x3C8 + 0x13C + level number x 0x44 + 0x3C, written once in the front end
        import dm2routes
        at = dm2routes.CAREER + rider * 0x3C8 + 0x13C + dm2routes.LEVEL_BLOCKS[level] * 0x44 + 0x3C
        poke['ZX_POKE_SCRIPT'] = '%x:%x=3' % (int(os.environ.get('COMP_POKE_TICK', '3000')), at)
    if level in XBOX_LEVELS:
        # (the app's [extras] dm2_xbox_levels) the Xbox levels' folder as the app bundles it (make-app.sh's dm2_xbox.py):
        # menu values 9, 10 in Free Ride, after Airport
        poke['ZX_DM2_XBOX_LEVELS_DIR'] = os.environ['DM2_XBOX_LEVELS_DIR']
        poke['ZX_DM2_XBOX_LEVELS'] = '1'
    if os.environ.get('SP_CARD') == 'native':
        poke['ZX_SAVES'] = None          # the native save file (run_config's card unset)
        poke['ZX_TRACE_SAVES'] = '1'
    cfg = run_config(name, menu.text(), env=poke, max_secs=max_secs, gs=gs)
    meter, log = Meter(), player_dm2.Log(os.path.join(d, 'play.log'))
    lv = dm2map.Level(XBOX_LEVELS.get(level) or dm2map.LEVELS[level])
    pl = player_dm2.Player(lv, seed, log)
    pl.explorer = mode == 'freeride'
    pl.line_share = 0.9 if mode != 'freeride' else 0.1
    rec = core.Recorder()
    trace = core.Trace(os.path.join(d, 'trace.txt'))
    start = {}
    dump_every = int(os.environ.get('DUMP_EVERY', '0'))
    dump_ticks = {int(x) for x in os.environ.get('DUMP_TICKS', '').split(',') if x}
    shots = sorted(shots)
    picture = []
    mp_screens = (multiplayer and os.environ.get('MP_SCREENS') == '1') or mode == 'competition'
    sp_card_on = not multiplayer and os.environ.get('SP_CARD') in ('1', 'native')
    native_saves = os.environ.get('SP_CARD') == 'native'     # (SP_CARD=native: the game saved to its native file)
    mp_idle = int(os.environ['MP_IDLE']) if os.environ.get('MP_IDLE') else None
    # PQ_RUNS=N: a ProQuest session of N runs of the level (the game over's ANOTHER RUN), each run's challenges the tier
    # the last one opened; ticks counts all of them
    pq_runs = int(os.environ.get('PQ_RUNS', '1')) if mode == 'proquest' else 1
    shot_screens = gs and mp_screens
    if os.environ.get('MP_OVER'):
        start['over_lines'] = [int(x) for x in os.environ['MP_OVER'].split(',')]
    if os.environ.get('MP_ROUND'):
        start['round_lines'] = [int(x) for x in os.environ['MP_ROUND'].split(',')]
    if os.environ.get('MP_TURN'):
        start['turn_lines'] = [int(x) for x in os.environ['MP_TURN'].split(',')]

    def program(*items):
        # a menu program: a button pressed 6 frames then `after` frames, or an int: frames of nothing
        out = []
        for it in items:
            if isinstance(it, int):
                out += [core.NOTHING] * it
            else:
                out += [core.pad(it)] * 6 + [core.NOTHING] * 30
        return out

    def keyboard_end():
        # a DM2 keyboard (4 rows of 12, SPC DEL END under them): a letter, END (LEFT wraps to the last column), accepted
        return program('CROSS', 'LEFT', 'DOWN', 'DOWN', 'DOWN', 'DOWN', 'CROSS', 200)

    def replay_file():
        # the native replay slot 1's file (games/dm2/host/slot_saves.cpp): its record (0x20: flags, name), then the
        # replay (0x2000)
        return os.path.join(run.dir, 'mc0-saves', 'Replays', 'Replay 01.sav')

    def replay_loaded(st):
        # the replay watched: the game's g_loadedReplay (the load's data) against the file's replay
        path = replay_file()
        data = open(path, 'rb').read() if os.path.exists(path) else b''
        loaded = bytes(run.mem(LOADED_REPLAY, 0x2000))
        same = len(data) == 0x2020 and data[0x20:] == loaded
        meter.s.count('replay_loaded_same' if same else 'replay_loaded_differs')
        log(st, 'saves: %s %d bytes, name %r; g_loadedReplay %s its replay' % (
            os.path.basename(path), len(data), data[4:0x20].split(b'\0')[0].decode('latin1'), 'is' if same else 'is NOT'))

    def sp_card(st):
        # (routes3, SP_CARD=1) a single-player run's end closed on the screen (g_gameScreen 0x25BBE4): RUN FINISHED's
        # button; the game over (10: ANOTHER RUN, LEVEL SELECT, SAVE GAME, SAVE REPLAY, VIEW REPLAY, TRICK LIST, VIEW
        # RECORDS, EXIT TO MAIN MENU; its cursor back on the first each time it is shown) in turn: SAVE REPLAY (12: slot 1,
        # the first replay slot, a name), SAVE GAME (11: slot 1, SAVE), EXIT TO MAIN MENU (YES); then the front end:
        # COOL STUFF, VIEW REPLAYS, the replay loaded from the card and watched, START out, back; OPTIONS, LOAD GAME.
        screen = run.mem(0x25BBE4, 1)[0]
        if (screen, st.mode) != start.get('sp'):
            log(st, 'screens: screen %d mode %d' % (screen, st.mode))
            start['sp'] = (screen, st.mode)
            start['fresh'] = True
            if gs:
                picture.append(run.shot('s%d-%d-%d' % (st.script, screen, st.mode)))
        if start.get('prog'):
            p = start['prog'].pop(0)
            return p
        if st.mode == 1 and native_saves:
            if not start.get('fe_done'):
                # (native: no card slot to choose) COOL STUFF, VIEW REPLAYS, the replay list, its first slot loaded from
                # its file and watched, START out, back
                start['fe_done'] = True
                log(st, 'screens: the front end, the replay loaded')
                start['prog'] = program(900, *['DOWN'] * 5, 'CROSS', 600, 'DOWN', 'DOWN', 'DOWN', 'CROSS', 900, 'CROSS', 2400,
                                        'START', 900, 'TRIANGLE', 900)
                start['replay_check'] = True
                return start['prog'].pop(0)
            if start.pop('replay_check', False):
                replay_loaded(st)
            start['fe'] = start.get('fe', 0) + 1
            return None if start['fe'] > 600 else core.NOTHING
        if st.mode == 1:
            if not start.get('fe_done'):
                start['fe_done'] = True
                log(st, 'screens: the front end, the replay loaded and the game loaded')
                start['prog'] = program(900, *['DOWN'] * 5, 'CROSS', 600, 'DOWN', 'DOWN', 'DOWN', 'CROSS', 900, 'CROSS', 1500,
                                        'CROSS', 2400, 'START', 900, 'TRIANGLE', 900, 'UP', 'CROSS', 300, 'DOWN', 'DOWN', 'DOWN',
                                        'CROSS', 900, 'CROSS', 900, 'UP', 'CROSS', 1500, 'CROSS', 600, 'TRIANGLE', 600)
                return start['prog'].pop(0)
            return None
        if not start.get('fresh'):
            return core.pad('CROSS') if screen == 0 and st.script % 60 < 6 and start.get('over') else core.NOTHING
        start['fresh'] = False
        if screen == 10:
            k = start.setdefault('card_visits', 0)
            start['card_visits'] += 1
            log(st, 'screens: game over, visit', k)
            if native_saves:
                # (SP_CARD=native: the native save files, no card dialogs) SAVE REPLAY (12: the replay list at once, its
                # first slot, a name, SAVE COMPLETE), SAVE GAME at once, then EXIT TO MAIN MENU (YES)
                if k == 0:
                    start['prog'] = program(90, 'DOWN', 'DOWN', 'DOWN', 'CROSS', 900, 'CROSS', 900) + keyboard_end() + program(600, 'CROSS', 600)
                elif k == 1:
                    start['prog'] = program(90, 'DOWN', 'DOWN', 'CROSS', 900)
                else:
                    start['prog'] = program(90, 'UP', 'CROSS', 150, 'DOWN', 'CROSS', 300)
                return start['prog'].pop(0)
            if k == 0:
                start['prog'] = program(90, 'DOWN', 'DOWN', 'DOWN', 'CROSS', 900, 'CROSS', 1500, 'CROSS', 900) + keyboard_end() + program(1500, 'CROSS', 600)
            elif k == 1:
                start['prog'] = program(90, 'DOWN', 'DOWN', 'CROSS', 900, 'CROSS', 900, 'UP', 'CROSS', 1500, 'CROSS', 600)
            else:
                start['prog'] = program(90, 'UP', 'CROSS', 150, 'DOWN', 'CROSS', 300)
            return start['prog'].pop(0)
        if screen == 18:
            start['prog'] = program(60, *['RIGHT'] * 11, *['DOWN'] * 4, 'CROSS', 240)
            return start['prog'].pop(0)
        start['fresh'] = screen not in (0, 11, 12)
        return core.pad('CROSS') if screen not in (0, 11, 12) and st.script % 60 < 6 else core.NOTHING

    def pq_flow(st):
        # (PQ_RUNS) the run's end to the next run, closed on the game's screen (g_gameScreen 0x25BBE4), each change
        # logged: a record's name (18, the keyboard) to END as SP_CARD's; the game over (10) its first line, ANOTHER RUN;
        # any other screen CROSS; the rider riding again with no screen up: the next run of the level.
        screen = run.mem(0x25BBE4, 1)[0]
        if (screen, st.mode, st.riding) != start.get('pq_screen'):
            start['pq_screen'] = (screen, st.mode, st.riding)
            log(st, 'proquest: screen %d mode %d riding %d' % (screen, st.mode, st.riding))
            if screen == 10:
                start['pq_prog'] = program(90, 'CROSS', 300)
        if start.get('pq_prog'):
            return start['pq_prog'].pop(0)
        start['pq_wait'] = start.get('pq_wait', 0) + 1
        if screen == 18:
            # The keyboard (12 columns of 4 rows, SPC DEL END under them): its cursor starts on A for the first record
            # and elsewhere for the next (seen: G), so ways to END in turn while the keyboard stays up.
            k = start['pq_kb'] = start.get('pq_kb', -1) + 1
            ways = ((['RIGHT'] * 11 + ['DOWN'] * 4), (['DOWN'] * 6 + ['RIGHT'] * 2), (['DOWN'] * 6 + ['RIGHT'] * 4),
                    (['LEFT'] * 12 + ['RIGHT'] * 11 + ['DOWN'] * 4))
            log(st, 'proquest: the keyboard, way %d' % (k % len(ways)))
            start['pq_prog'] = program(60, *ways[k % len(ways)], 'CROSS', 300)
            return start['pq_prog'].pop(0)
        if screen == 0 and st.riding and start['pq_wait'] > 300 and core.dist2(st.pos, start['pq_end_pos']) > 1.0:
            # (the run's end leaves the rider where the clock stopped; the next run puts it at the level's start)
            start['runs'] = start.get('runs', 1) + 1
            start['pq_live'] = False
            start['same'] = start['rounds'] = start['pq_wait'] = start['over'] = 0
            meter.s.count('runs')
            rec = level_record(run, st)
            meter.s.best('goals', goals_done(rec))
            log(st, 'run %d: challenges done %d, tier %d' % (start['runs'], goals_done(rec), rec[0x3D]))
            return None
        if start['pq_wait'] > 6000:
            return 'end'
        return core.pad('CROSS') if screen != 0 and st.script % 90 < 6 else core.NOTHING

    def policy(st):
        meter.tick(st)
        if sp_card_on and 't' in start and (start.get('over') or not st.riding or start.get('sp_live')):
            start['sp_live'] = True
            return sp_card(st)
        if 't' in start and mp_screens:
            # (routes3) closed on the game's screen (g_gameScreen 0x25BBE4, its low byte) and the player whose turn it is
            # (g_currentPlayer 0x2566CC): a screen's first line every 90 ticks (CONTINUE, the results); MP_IDLE's player
            # rides nothing (a HORSE turn not matched: a letter); the end as below.
            screen = run.mem(0x25BBE4, 1)[0]
            player = run.mem(0x2566CC, 1)[0]
            if (screen, player) != start.get('sp'):
                log(st, 'multiplayer: screen %d player %d score %.0f' % (screen, player, st.run_score))
                start['sp'] = (screen, player)
                start['fresh'] = True
                if screen in (10, 25, 26):
                    meter.s.count('screen_%d' % screen)
                if shot_screens:
                    picture.append(run.shot('s%d-%d-%d' % (st.script, screen, player)))
            if st.mode == 1:
                start['fe'] = start.get('fe', 0) + 1
                return None if start['fe'] > 600 else core.NOTHING
            if screen == 23 and start.get('over_done'):
                return None                       # (MP_OVER's last line was EXIT TO GAME SELECT: the session ends here)
            if start.get('replay_until') and st.script < start['replay_until']:
                return core.NOTHING               # a replay shown (VIEW REPLAY): watched, then any screen's CROSS ends it
            lines = start.get('over_lines') if screen == 10 else start.get('turn_lines') if screen == 26 else \
                start.get('round_lines') if screen == 9 else None
            if lines and lines[0] == 99 and start.get('fresh'):
                return None                       # (MP_OVER's 99: the session ends on this screen)
            if lines is not None and ((lines and start.get('fresh')) or start.get('over_prog')):
                # MP_OVER (the game over, 10: PLAY AGAIN, VIEW REPLAY, VIEW RECORDS, EXIT TO GAME SELECT) and MP_TURN (the time
                # up, 26: CONTINUE, VIEW REPLAY, EXIT): DOWN presses before each visit's CROSS, a list taken in turn
                if not start.get('over_prog'):
                    start['fresh'] = False
                    downs = lines.pop(0)
                    log(st, 'multiplayer: screen %d line %d' % (screen, downs))
                    start['over_prog'] = ([core.NOTHING] * 90 + sum(([core.pad('DOWN')] * 6 + [core.NOTHING] * 30 for _ in range(downs)), [])
                                          + [core.pad('CROSS')] * 6 + [core.NOTHING] * 30)
                    if (downs == 1 and screen != 9) or (downs == 2 and screen == 9):
                        start['replay_until'] = st.script + len(start['over_prog']) + 900
                    if screen == 10 and downs == 3 and not lines:
                        start['over_done'] = True
                p = start['over_prog'].pop(0)
                if not start['over_prog']:
                    start['over_prog'] = None
                return p
            start['over_prog'] = None
            if screen == 18:
                # a record's name (the keyboard): END, the grid's bottom right corner, as the name entry below
                if not start.get('name_prog'):
                    press = lambda b: [core.pad(b)] * 6 + [core.NOTHING] * 14
                    start['name_prog'] = [core.NOTHING] * 60 + sum((press(b) for b in ['RIGHT'] * 11 + ['DOWN'] * 4 + ['CROSS']), []) + [core.NOTHING] * 240
                p = start['name_prog'].pop(0)
                if not start['name_prog']:
                    start['name_prog'] = None
                return p
            start['name_prog'] = None
            if screen in (3, 4, 5, 6, 19, 21, 22):
                # the records asked for (pause, round over, game over): CROSS is the next page; TRIANGLE goes back
                k = st.script % 180
                return core.pad('CROSS') if k < 6 and start.get('paged') != screen else core.pad('TRIANGLE') if 90 <= k < 96 else \
                    (start.__setitem__('paged', screen) or core.NOTHING) if k == 6 else core.NOTHING
            start['paged'] = None
            if screen != 0:
                return core.pad('CROSS') if st.script % 90 < 6 else core.NOTHING
            if mp_idle is not None and player == mp_idle and st.riding:
                return core.NOTHING
        if multiplayer and 't' in start and not st.riding:
            # Between turns and at the game's end the results screens wait for a button; the game is over when the front
            # end is back (mode 1) for a while.
            start['fe'] = start.get('fe', 0) + 1 if st.mode == 1 else 0
            if start['fe'] == 1:
                log(st, 'multiplayer: the front end again, session %08x' % st.session)
                meter.s.count('finished')
            return None if start['fe'] > 600 else (core.pad('CROSS') if st.script % 90 < 6 else core.NOTHING)
        if 't' in start and not st.riding:
            # The run ended: the game left the level by itself (RUN FINISHED): a few ticks more, then stop.
            start['over'] = start.get('over', 0) + 1
            if start['over'] == 1:
                log(st, 'run over: mode', st.mode, 'frame', st.frame, 'session %08x' % st.session, 'rider %x' % st.rider)
            if start.get('pq_live') and start['over'] <= 6000:
                p = pq_flow(st)                   # (PQ_RUNS) the next run loading
                return None if p == 'end' else p or core.NOTHING
            return None if start['over'] > 900 else core.NOTHING
        if not st.riding or st.script < menu.t + 30:
            return core.NOTHING
        start.setdefault('t', st.script)
        t = st.script - start['t']
        if (dump_every and t % dump_every == 0) or t in dump_ticks:
            # (DUMP_EVERY, DUMP_TICKS) the RAM every so many ticks of play, and at the ticks listed: the panel's shots of a run (zx-panelshot --game dm2)
            with open(os.path.join(d, 'tick-%d.ram' % t), 'wb') as f:
                f.write(run.mem(0, 0x2000000))
        if start.get('pq_live'):
            p = pq_flow(st)
            if p == 'end':
                return None
            if p is not None:
                return p
        if t > ticks:
            return None
        if t % 300 == 0:
            meter.s.best('goals', goals_done(level_record(run, st)))
            if mode == 'proquest':
                pl.targets = challenge_targets(run, st)
                if meter.s['goals']:
                    pl.line_share = 0.5
        if shots and t >= shots[0]:
            picture.append(run.shot('t%d' % shots.pop(0)))
        # The run's time is up: the rider stands in mid-move, the clock goes on, the game waits for a button (RUN FINISHED).
        moving = any(abs(v) > 0.001 for v in st.vel)
        start['same'] = start.get('same', 0) + 1 if st.pos == start.get('pos') and moving else 0
        start['pos'] = st.pos
        if start.get('names'):
            return start['names'].pop(0)
        if start['same'] > 200 and mode == 'freeride':       # no end to a free ride: wedged, the bail button
            start['same'] = 0
            log(st, 'wedged')
            meter.s.count('wedged')
            pl.bail_now = 8
        if start['same'] > 200 and multiplayer:
            # A turn's time is up (the rider stands, the clock goes on): the turn's and the round's results, CROSS.
            if start['same'] == 201:
                log(st, 'multiplayer: turn over, score', st.run_score)
                meter.s.count('turns')
            return core.pad('CROSS') if t % 60 < 6 else core.NOTHING
        if start['same'] > 200 and sp_card_on:
            start['over'] = 1                    # (routes3) the run's end: SP_CARD's screens from here
            log(st, 'run finished: score', st.run_score)
            meter.s.count('finished')
            return core.NOTHING
        if start['same'] > 200 and start.get('runs', 1) < pq_runs:
            start['pq_live'] = True               # (PQ_RUNS) the run's end: pq_flow to the next run
            start['pq_end_pos'] = st.pos
            log(st, 'run finished: score', st.run_score)
            return core.NOTHING
        if start['same'] > 200:
            # A new record stops the run for the name: to END (the grid's bottom right corner) and CROSS, up to three names.
            if start.get('rounds', 0) < 3:
                start['rounds'] = start.get('rounds', 0) + 1
                start['same'] = 0
                log(st, 'name entry', start['rounds'], 'score', st.run_score)
                meter.s.count('records')
                press = lambda b: [core.pad(b)] * 6 + [core.NOTHING] * 14
                start['names'] = sum((press(b) for b in ['RIGHT'] * 11 + ['DOWN'] * 4 + ['CROSS']), []) + [core.NOTHING] * 120
                return core.NOTHING
            if 'done' not in start:
                start['done'] = t
                meter.s.count('finished')
                meter.s.best('goals', goals_done(level_record(run, st)))
                log(st, 'run finished: score', st.run_score)
            if t > start['done'] + 400:
                return None
            return core.pad('CROSS') if t % 60 < 6 else core.NOTHING
        if not moving:
            start['rounds'] = 0
        return pl(st)

    def ending(st):
        p = policy(st)
        if p is None:
            start['career'] = career_summary(run)      # (while the game waits on this read)
        return p

    with core.Run(cfg) as run:
        core.play(run, read, ending, max_ticks=ticks + menu.t + 60000, recorder=rec, trace=trace)
    trace.close()
    text = menu.text() + ',' + rec.text()
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(text)
    with open(os.path.join(d, 'route.txt'), 'w') as f:      # dm2routes' played route
        f.write(text)
    stats = meter.s.json()
    meta = dict(name=name, mode=mode, level=level, rider=rider, bike=bike, seed=seed, ticks=ticks, end=run.script_time, end_tick=run.tick, menu_end=menu.t, env=poke)
    stats['areas'] = len(pl.seen)
    stats['areas_all'] = len(pl.floor_cells or ())
    stats['trick_ids'] = sorted(meter.tricks)
    stats['career'] = start.get('career')
    core.save_json(os.path.join(d, 'stats.json'), stats)
    core.save_json(os.path.join(d, 'meta.json'), meta)
    print(name, meter.s.row(COLUMNS), 'areas', len(pl.seen))


def reload(name, saves, profile=None):
    """A native save loaded: the front end's OPTIONS, LOAD GAME (native saves: no card dialog), then the career as the
    game holds it (career_summary), before and after the load. SAVES: the folder of <profile>.sav (a played run's
    runs/NAME/mc0-saves)."""
    import dm2routes
    s = dm2routes.Script()
    s.wait(300)
    s.presses(*['DOWN'] * dm2routes.MAIN['options'])
    s.press('CROSS', after=120)          # OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=600)          # LOAD GAME
    s.wait(600)
    env = {'ZX_SAVES': None, 'ZX_SAVES_DIR': saves, 'ZX_TRACE_SAVES': '1'}
    if profile:
        env['ZX_SAVE_PROFILE'] = profile
    cfg = run_config(name, s.text(), env=env, max_secs=600)
    out = {}
    before = s.t - 1400

    def policy(st):
        if 'before' not in out and st.script >= before:
            out['before'] = career_summary(run)
        if st.script >= s.t:
            out['after'] = career_summary(run)
            return None
        return core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=s.t + 30000)
    print(name, 'profile', profile or 'Career', 'before', json.dumps({k: out.get('before', {}).get(k) for k in ('goals', 'digest')}),
          'after', json.dumps(out.get('after')), flush=True)
    return out


def watch(name, saves, card=None):
    """A native replay watched: the front end's COOL STUFF, VIEW REPLAYS (native saves: the replay list at once), the
    list's first slot loaded and shown; the list's names and g_loadedReplay against Replays/Replay 01.sav's replay.
    SAVES: the saves folder; CARD: a memory card folder for slot 1 (ZX_MC_ROOT: a card whose replay library the
    first list brings in)."""
    import dm2routes
    s = dm2routes.Script()
    s.wait(300)
    s.presses(*['DOWN'] * dm2routes.MAIN['cool'])
    s.press('CROSS', after=600)          # COOL STUFF
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    s.press('CROSS', after=900)          # VIEW REPLAYS: the list
    listed = s.t - 60
    s.press('CROSS', after=2400)         # the first slot: loaded, then shown
    env = {'ZX_SAVES': None, 'ZX_SAVES_DIR': saves, 'ZX_TRACE_SAVES': '1'}
    if card:
        env['ZX_MC_ROOT'] = card
    cfg = run_config(name, s.text(), env=env, max_secs=600)
    out = {'modes': []}
    slot = _dm2_symbol('g_replaySlots')

    def policy(st):
        if not out['modes'] or out['modes'][-1][1] != st.mode:
            out['modes'].append((st.script, st.mode))
        if 'list' not in out and st.script >= listed:
            data = run.mem(slot, 8 * 0x20)
            out['list'] = [(i + 1, data[i * 0x20 + 4:i * 0x20 + 0x20].split(b'\0')[0].decode('latin1'))
                           for i in range(8) if data[i * 0x20] & 1]
        if st.script >= s.t:
            out['loaded'] = bytes(run.mem(LOADED_REPLAY, 0x2000))
            return None
        return core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=s.t + 30000)
    path = os.path.join(saves, 'Replays', 'Replay 01.sav')
    data = open(path, 'rb').read() if os.path.exists(path) else b''
    same = len(data) == 0x2020 and out.get('loaded') == data[0x20:]
    print(name, 'list', out.get('list'), 'modes', out['modes'], 'g_loadedReplay', 'is' if same else 'is NOT',
          'Replay 01.sav\'s replay', flush=True)
    return dict(list=out.get('list'), modes=out['modes'], same=same)


def replay(name, suffix='replay', spec=None, runner=None):
    """The session's script alone (no decisions): the same trace, tick for tick? With a spec, the check of readable code
    against the played run: a checkpoint at every tick (the rider's state, the score, the run counters, the mode)."""
    d = session_dir(name)
    meta = json.load(open(os.path.join(d, 'meta.json')))
    script = open(os.path.join(d, 'script.txt')).read()
    env = dict(meta.get('env', {}))
    if spec:
        env['ZX_READABLE'] = spec if not spec.startswith('@') else 'tested,' + ','.join(open(spec[1:]).read().split())
    cfg = run_config(name + '-' + suffix, script, env=env, max_secs=1200)
    if runner:
        cfg.runner = runner
    trace = core.Trace(os.path.join(d, 'trace-%s.txt' % suffix))
    meter = Meter()

    def policy(st):
        meter.tick(st)
        if st.script >= meta['end']:
            return None
        return core.NOTHING

    # The trace of the played run has one line per decision from the first riding tick on: every read here too.
    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=meta['end'] + 60000, trace=trace)
    trace.close()
    diff = core.compare_traces(os.path.join(d, 'trace.txt'), os.path.join(d, 'trace-%s.txt' % suffix))
    print(name, 'replay', 'IDENTICAL' if diff is None else 'DIFFERS at line %d: %s | %s' % diff, meter.s.row(COLUMNS))


def states(name, spec, first, last, words=None):
    """The session's script under readable SPEC (off, or '' the defaults): the rider's state fields at each script time
    in FIRST..LAST (the digest's parts, field by field) and WORDS (ADDR[+N],...: more RAM), into
    sessions/NAME/states-SPEC.json; two of them say which field parts first."""
    d = session_dir(name)
    meta = json.load(open(os.path.join(d, 'meta.json')))
    script = open(os.path.join(d, 'script.txt')).read()
    env = dict(meta.get('env', {}), ZX_READABLE=spec)
    cfg = run_config(name + '-states-' + (spec or 'defaults'), script, env=env, max_secs=1200)
    extra = []
    for w in filter(None, (words or '').split(',')):
        a, _, n = w.partition('+')
        extra.append((int(a, 16), int(n or '4', 0)))
    out = {}

    def policy(st):
        if first <= st.script <= last:
            row = dict(mode=st.mode, frame=st.frame, flags=st.flags, actions=st.actions, action_frames=st.action_frames,
                       trick=st.trick, bail=st.bail_kind, score=st.run_score, line=st.line_score, grind=st.grind_time,
                       line_state=st.line_state, line_tricks=st.line_tricks, counts=st.counts, physics=st.physics,
                       up=[x.hex() for x in st.up], fwd=[x.hex() for x in st.fwd], pos=[x.hex() for x in st.pos],
                       balance=st.balance.hex(), vel=[x.hex() for x in st.vel], speed=st.speed.hex())
            if st.rider:
                row['rider_hex'] = run.mem(st.rider, 0x1100).hex()
            for a, n in extra:
                row['%x' % a] = run.mem(a, n).hex()
            out[st.script] = row
        return None if st.script > last else core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=last + 60000)
    path = os.path.join(d, 'states-%s.json' % (spec or 'defaults'))
    core.save_json(path, out)
    print(name, spec or 'defaults', len(out), 'states', path)


def states_diff(name, a='defaults', b='off'):
    """The first script time and fields where two states files differ (the rider's bytes by offset)."""
    d = session_dir(name)
    A = json.load(open(os.path.join(d, 'states-%s.json' % a)))
    B = json.load(open(os.path.join(d, 'states-%s.json' % b)))
    for t in sorted(A, key=int):
        if t not in B or A[t] == B[t]:
            continue
        fields = [k for k in A[t] if A[t].get(k) != B[t].get(k)]
        print('script', t, 'differs in', fields)
        if 'rider_hex' in fields:
            x, y = bytes.fromhex(A[t]['rider_hex']), bytes.fromhex(B[t]['rider_hex'])
            offs = [i for i in range(0, min(len(x), len(y)), 4) if x[i:i + 4] != y[i:i + 4]]
            print('  rider words differ at', ['+%#x %s|%s' % (o, x[o:o + 4].hex(), y[o:o + 4].hex()) for o in offs[:40]])
        for k in fields:
            if k != 'rider_hex':
                print('  %s: %s | %s' % (k, A[t][k], B[t][k]))
        return t
    print('no difference')


def pictures(name, times, out):
    """The session's script with the GS on: a picture at each script time, in folder `out`."""
    d = session_dir(name)
    meta = json.load(open(os.path.join(d, 'meta.json')))
    script = open(os.path.join(d, 'script.txt')).read()
    cfg = run_config(name + '-gs', script, env=dict(meta.get('env', {})), max_secs=1200, gs=True)
    times = sorted(times)
    last = times[-1] + 30

    def policy(st):
        while times and st.script >= times[0]:
            run.shot('t%d' % times.pop(0))
        return None if st.script >= last else core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, read, policy, max_ticks=last + 60000)
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(os.path.join(run.dir, 'shots')):
        os.replace(os.path.join(run.dir, 'shots', f), os.path.join(out, name + '-' + f))


def main():
    a = sys.argv[1:]
    if a and a[0] == 'measure':
        for r in a[1:]:
            measure(r)
    elif a and a[0] == 'play':
        o = dict(kv.split('=', 1) for kv in a[2:])
        play_session(a[1], o.get('mode', 'freeride'), int(o.get('level', 0)), int(o.get('rider', 0)), int(o.get('bike', 0)),
                     int(o.get('seed', 1)), int(o.get('ticks', 10800)), o.get('gs') == '1',
                     [int(x) for x in o.get('shots', '').split(',') if x])
    elif a and a[0] == 'replay':
        for n in a[1:]:
            replay(n)
    elif a and a[0] == 'states':        # states NAME SPEC FIRST LAST [WORDS]: fields per tick under SPEC ('' defaults)
        states(a[1], a[2], int(a[3]), int(a[4]), a[5] if len(a) > 5 else None)
    elif a and a[0] == 'statesdiff':    # statesdiff NAME [A B]
        states_diff(a[1], *a[2:4])
    elif a and a[0] == 'reload':        # reload NAME SAVES_DIR [PROFILE]: a native save loaded from the front end
        reload(a[1], a[2], a[3] if len(a) > 3 else None)
    elif a and a[0] == 'watch':         # watch NAME SAVES_DIR [CARD]: a native replay watched from the front end
        watch(a[1], a[2], a[3] if len(a) > 3 else None)
    elif a and a[0] == 'shots':        # shots NAME T,T,...: pictures at script times, into scratch shots/
        pictures(a[1], [int(x) for x in a[2].split(',')], SCRATCH + '/shots')
    elif a and a[0] == 'check':        # check SPEC NAME ...: readable SPEC against the played run, tick by tick
        for n in a[2:]:
            replay(n, 'spec-' + os.path.basename(a[1]).split('.')[0], a[1])


if __name__ == '__main__':
    main()
