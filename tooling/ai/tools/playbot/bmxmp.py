#!/usr/bin/env python3
"""BMX XXX's multiplayer, played (routes3): two riders on one split screen, port 1 a copy of port 0 (ZX_PAD_PORT1=mirror:
the second pad the MULTIPLAYER item asks for), so both riders take the player's answers.

    [LEVEL=n RIGHTS=k] bmxmp.py play NAME SEED GAMES [TICKS]   (LEVEL: bmx.LEVELS' index, its cheat entered first;
                                                                RIGHTS: presses on the MP level select)

GAMES: the modes in the order played, comma separated: skillz, paintball, strip (the retail SELECT GAME's three; its
table holds no B-M-X, HIGH FIVE or RANDOM SPOT), and the three-chance contests grind (LONGEST GRIND, mode 9) and walltap
(HIGHEST WALLTAP, mode 10), which the retail list does not offer: its first line's mode (g_mpGameSelectModes) is poked
to the contest's as SELECT GAME comes up (extras3's way), then that line is taken. A contest is three rounds of a 30 s
turn each, the players in turn (the rider of the player whose turn it is is read and ridden); every turn starts at the
level's start, and the player (bmx.player_of, contest) plans again from there: to the nearest rail way for grind, to the
nearest tall wall of the level's map with a run in along it for walltap. A game ends on the game's own clock (SKILLZ 2 minutes, PAINTBALL 90 s a
side) or, for STRIP CHALLENGE (no clock), after TICKS of play by the pause menu's END RUN. The screens between are
answered closed on g_gameScreen (0x2A2D70): SELECT GAME (37) moves through the modes once, then picks the next game;
the instructions (25) CONTINUE; RUN FINISHED (40) once EXIT and NO, then CONTINUE; the game over (22) once VIEW RECORDS,
once PLAY AGAIN, then EXIT TO GAME SELECT; after the last game EXIT TO MAIN MENU, YES. One pause tour (VIEW RECORDS,
OPTIONS) in the first game. Riding is bmx.py's player on rider 0 (CHL_MULY, the level the select starts on); in
PAINTBALL the scope (the other pad's) fires with CIRCLE every FIRE_EVERY ticks of play.
sessions/NAME as bmx.py play: script.txt (the replay), meta.json (env with the mirror), stats.json, play.log.
"""
import os
import struct
import sys

CORE = os.environ.get('PLAYBOT_CORE', os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, CORE)
import core  # noqa: E402
import bmx  # noqa: E402

SCREEN = 0x2A2D70        # g_gameScreen
RIDER_TABLE = bmx.RIDER_TABLE
PLAY, PAUSE, INSTRUCTIONS, GAME_OVER, RECORDS, AFTER_GAME, AFTER_TURN = 1, 7, 25, 22, 33, 37, 40
EXIT_GAME, EXIT_LEVEL = 17, 18
MODES = ('skillz', 'paintball', 'strip')     # SELECT GAME's lines 0..2; 3 EXIT TO MAIN MENU
# (routes2) SELECT GAME's size and its EXIT line, and whether the ridden rider is the turn's in every mode with turns
# (else in the contests only): bmxmpmodes.py sets them for the all-modes extra's longer list
SELECT_LINES, SELECT_EXIT = 4, 3
TURN_RIDER_ALWAYS = False
CONTESTS = {'grind': 9, 'walltap': 10}       # g_gameModes' rows: their records DISTANCE_GRIND (8), DISTANCE_TAP (7)
MODE_SELECT = 0x29ECF8   # g_mpGameSelectModes: SELECT GAME's lines' modes (retail 4, 17, 18, 0)
CURRENT_PLAYER = 0x29C5E3  # g_currentPlayer (u8): whose turn it is
GAME_MODE = 0x29EC57     # g_currentGameMode (u8)
GAME_MODES = 0x284300    # g_gameModes: 0x14 bytes a mode, flags at +0x04 (kModeMultiplayer 0x2, kModeTurns 0x4)
FIRE_EVERY = int(os.environ.get('FIRE_EVERY', '45'))
# (routes3) GAMEOVER_REPLAY=1 (with ZX_BMX_MENU_LINES=1, which adds the game over's VIEW REPLAY): at each game's game
# over the VIEW REPLAY line is taken once by g_menuChoices (the lines the extra adds move the others), the shown replay
# (the round's: shownReplay) and each player's replay logged with their set-up byte (Replay::playerWatches: 1 for
# PAINTBALL's sniper), the replay watched to its end with CROSS (the next shot) pressed until the rider-eyes shot (11),
# then EXIT TO GAME SELECT by g_menuChoices.
GAMEOVER_REPLAY = os.environ.get('GAMEOVER_REPLAY') == '1'
REPLAY = 2
GAME_OVER_PLAY_AGAIN, GAME_OVER_VIEW_REPLAY, GAME_OVER_EXIT = 0, 5, 8      # screen_game_over.cpp GameOverChoice
REPLAY_WATCHES, REPLAY_RIDER, CAMERA_SHOT, SHOT_RIDER_EYES = 0x245, 0x03, 0x03, 11


def menu_script(level=0, rights=0):
    """Title, main menu, (the level's cheat, as bmx.menu_script), MULTIPLAYER (a stop now: two pads), RIDER 1 and RIDER 2
    (the first), the level select (RIGHT `rights` times), loading: closed on MODE at the start, as bmx.menu_script."""
    import bmxroutes
    s = bmxroutes.Script()
    code = bmx.LEVELS[level][1]
    if code:
        bmxroutes.cheats(s, [code])
    s.press('DOWN', after=60)
    s.press('CROSS', after=600)          # MULTIPLAYER: RIDER 1 SELECT
    s.press('CROSS', after=600)          # RIDER 2 SELECT
    s.press('CROSS', after=600)          # the level select
    s.presses(*['RIGHT'] * rights, after=120)
    s.press('CROSS', after=300)          # the level: loading
    return s


def play_mp(name, seed=1, games=('skillz',), ticks=5400, gs=False, max_secs=1500, level=0, rights=0):
    import dm2
    import bmxmap  # noqa: F401
    import dm2map
    import player_dm2
    single = os.environ.get('SINGLE') == '1'
    # (SINGLE=1: HARDCORE TOUR's way to the level, bmx.menu_script, one rider: the screens answered as below, unknown
    # ones with CROSS, with GS=1 a picture at each screen's change)
    menu = bmx.menu_script(level, 0) if single else menu_script(level, rights)
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    env = {'ZX_EE_SCALE': '4096'} if os.environ.get('SINGLE') == '1' else {'ZX_EE_SCALE': '4096', 'ZX_PAD_PORT1': 'mirror'}
    cfg = bmx.run_config(name, menu.text(), env=env, max_secs=max_secs, gs=gs)
    meter, log = dm2.Meter(), player_dm2.Log(os.path.join(d, 'play.log'))
    lv = dm2map.Level(bmx.LEVELS[level][0])
    pl = bmx.player_of(lv, seed, log)
    rec = core.Recorder()
    trace = core.Trace(os.path.join(d, 'trace.txt'))
    todo = list(games)
    v = {'screen': None, 'since': 0, 'cursor': {}, 'seen': set(), 'game': None, 'played': [], 'play_t': 0,
         'paused': False, 'turn_exit': False, 'over_records': False, 'again': False, 'fe': None, 'scores': [],
         'fired': 0, 'events': [], 'need': False, 'stable': 0, 'from_play': False}
    program = []
    sym = None
    if GAMEOVER_REPLAY:
        import goalsteer
        sym = goalsteer.Symbols('bmx')

    def go(screen, target, lines, wrap=True):
        """Presses that move a menu's cursor (kept per screen, as the game keeps it) to `target`, then CROSS."""
        at = v['cursor'].get(screen, 0)
        out = []
        n = (target - at) % lines if wrap else target - at
        if wrap and n > lines // 2:
            out += ['UP'] * (lines - n)
        else:
            out += ['DOWN'] * n if n >= 0 else ['UP'] * -n
        v['cursor'][screen] = target
        return out + ['CROSS']

    def queue(*items):
        program.extend(bmx.presses(*items))

    def scores(run):
        out = []
        for i in range(2):
            r = struct.unpack('<I', run.mem(bmx.RIDER_TABLE + 4 * i, 4))[0]
            out.append(round(struct.unpack('<f', run.mem(r + 0x1C0, 4))[0]) if 0x100000 <= r < 0x2000000 else None)
        return out

    def game_scores(run):
        """Each rider's ScoreBlock::gameScore (+0x1C4): a contest's best measure, in the game's units (the game over's
        16' 5" is 5)."""
        out = []
        for i in range(2):
            r = struct.unpack('<I', run.mem(bmx.RIDER_TABLE + 4 * i, 4))[0]
            out.append(struct.unpack('<I', run.mem(r + 0x1C4, 4))[0] if 0x100000 <= r < 0x2000000 else None)
        return out

    def turn_rider():
        """The rider ridden: the current player's in a mode whose players take turns, else player one's."""
        flags = struct.unpack('<I', run.mem(GAME_MODES + 0x14 * run.mem(GAME_MODE, 1)[0] + 4, 4))[0]
        return run.mem(CURRENT_PLAYER, 1)[0] & 1 if flags & 6 == 6 else 0

    def read(r):
        cur = turn_rider() if v['game'] in CONTESTS or TURN_RIDER_ALWAYS else 0
        bmx.RIDER_TABLE = RIDER_TABLE + 4 * cur
        try:
            return bmx.read(r)
        finally:
            bmx.RIDER_TABLE = RIDER_TABLE

    def replays_now():
        """The shown replay (shownReplay: the round's at a multiplayer game's end) and each player's: index, rider,
        playerWatches."""
        def one(i):
            r = run.u32(sym['g_replays'] + 4 * i)
            return (i, run.mem(r + REPLAY_RIDER, 1)[0], run.mem(r + REPLAY_WATCHES, 1)[0]) if r else (i, None, None)
        shown = run.mem(sym['g_roundReplay'], 1)[0]
        players = list(run.mem(sym['g_playerReplays'], 2))
        return 'round %s, players %s' % (one(shown), [one(i) for i in players])

    def replay_shots(st):
        """In the replay: CROSS (the next shot) until the replay camera's shot is the rider-eyes shot."""
        v['replay_frames'] = v.get('replay_frames', 0) + 1
        if v['replay_frames'] < 150:
            return core.NOTHING
        cams = [run.u32(sym['g_playerCameras'] + 4 * i) for i in range(2)]
        shots = [run.mem(c + CAMERA_SHOT, 1)[0] if c else None for c in cams]
        if shots != v.get('shots'):
            v['shots'], v['shot_at'] = shots, v['replay_frames']
            log(st, 'replay frame %d: camera shots %s' % (v['replay_frames'], shots))
        held = v['replay_frames'] - v['shot_at']
        if SHOT_RIDER_EYES in shots:
            return core.NOTHING
        return core.pad('CROSS') if held > 20 and held % 30 < 4 else core.NOTHING

    def on_screen(st, screen):
        game = v['game']
        if single:
            log(st, 'screen %d: CROSS' % screen)
            queue(60, 'CROSS')
            return
        if screen == AFTER_GAME:
            if 'select' not in v['seen']:
                v['seen'].add('select')
                queue(20, 'DOWN', 'DOWN', 'DOWN', 'UP', 'UP', 'UP')      # each line's description (OnChange)
            if todo:
                v['game'] = todo.pop(0)
                v['again'] = False
                log(st, 'game', v['game'])
                if v['game'] in CONTESTS:
                    run.command('poke %#x %d' % (MODE_SELECT, CONTESTS[v['game']]))
                    pl.contest = v['game']
                    queue(*go(AFTER_GAME, 0, 4))
                else:
                    pl.contest = None
                    queue(*go(AFTER_GAME, MODES.index(v['game']), SELECT_LINES))
            else:
                queue(*go(AFTER_GAME, SELECT_EXIT, SELECT_LINES), 120, 'DOWN', 'CROSS')       # EXIT TO MAIN MENU; EXIT? NO, YES
                v['fe'] = st.script
        elif screen == INSTRUCTIONS:
            log(st, 'instructions: game mode %d' % run.mem(GAME_MODE, 1)[0])     # (routes2) the mode the game runs
            queue(30, 'CROSS')
            v['play_t'] = 0
        elif screen == AFTER_TURN:
            if not v['turn_exit'] and game not in CONTESTS:
                v['turn_exit'] = True
                queue(60, *go(AFTER_TURN, 1, 2), 150, 'CROSS', 60)        # EXIT, then NO: back here
            else:
                queue(60, *go(AFTER_TURN, 0, 2))                         # CONTINUE
        elif screen == GAME_OVER and GAMEOVER_REPLAY:
            if v['from_play']:
                v['from_play'] = False
                v['scores'].append((game, v['last_scores']))
                log(st, 'game over', game, 'scores', v['last_scores'], 'game scores', game_scores(run))
            choices = list(run.mem(sym['g_menuChoices'], 16))
            if not v.get('replayed') and GAME_OVER_VIEW_REPLAY in choices:
                v['replayed'], v['replay_frames'] = True, 0
                log(st, 'game over choices %s; replays: %s' % (choices, replays_now()))
                queue(90, *['DOWN'] * choices.index(GAME_OVER_VIEW_REPLAY), 'CROSS', 30)
            elif GAME_OVER_EXIT in choices:
                v['replayed'] = False
                queue(90, *['DOWN'] * choices.index(GAME_OVER_EXIT), 'CROSS')
            elif v.get('over_reads', 0) < 20:
                # (the menu's lines are made as the screen comes in: read again a second later)
                v['over_reads'] = v.get('over_reads', 0) + 1
                v['need'], v['stable'] = True, st.script + 30
                return
            else:
                log(st, 'game over choices %s: CROSS' % choices)
                queue(90, 'CROSS')
            v['over_reads'] = 0
        elif screen == REPLAY and GAMEOVER_REPLAY:
            pass                                                         # (policy's own: the shots)
        elif screen == GAME_OVER:
            if v['from_play']:
                v['from_play'] = False
                v['scores'].append((game, v['last_scores']) if game not in CONTESTS else (game, v['last_scores'], game_scores(run)))
                log(st, 'game over', game, 'scores', v['last_scores'], 'game scores', game_scores(run))
                if gs:
                    v['shot_over'] = st.script + 50
            if not v['over_records']:
                v['over_records'] = True
                queue(90, *go(GAME_OVER, 1, 3), 150, 'RIGHT', 60, 'TRIANGLE', 120)   # VIEW RECORDS, its next page, back
            elif game == 'skillz' and not v['again']:
                v['again'] = True
                queue(60, *go(GAME_OVER, 0, 3))                          # PLAY AGAIN
            else:
                queue(60, *go(GAME_OVER, 2, 3))                          # EXIT TO GAME SELECT
        elif screen in (RECORDS, PAUSE, EXIT_GAME, EXIT_LEVEL):
            pass                                                         # (a program's own)
        elif screen != PLAY:
            log(st, 'screen %d: CROSS' % screen)
            queue(60, 'CROSS')

    def policy(st):
        meter.tick(st)
        if st.script < menu.t:
            return core.NOTHING
        if single and st.script - menu.t > ticks:
            return None
        if single and v['game'] and st.mode == 1 and v['fe'] is None:
            v['fe'] = st.script
        if v['fe'] is not None and st.mode == 1:
            return None if st.script - v['fe'] > 900 else core.NOTHING
        screen = struct.unpack('<I', run.mem(SCREEN, 4))[0]
        if gs and v.get('shot_over') and st.script >= v['shot_over']:
            v['shot_over'] = None
            log(st, 'shot', run.shot('over-%s-%d' % (v['game'], st.script)))
        if screen != v['screen']:
            log(st, 'screen %s -> %d' % (v['screen'], screen))
            v['events'].append((st.script, screen))
            if v['screen'] == PLAY:
                v['from_play'] = True
                if v['game'] in CONTESTS:
                    log(st, 'turn over: player %d, turn scores %s' % (run.mem(CURRENT_PLAYER, 1)[0], scores(run)))
            if screen == PLAY and v['game'] in CONTESTS:
                v['turn_start'] = True
            v['screen'] = screen
            v['need'], v['stable'] = True, st.script
            if gs:
                log(st, 'shot', run.shot('s%d-%d' % (st.script, screen)))
            if single and v['game'] is None:
                v['game'] = 'single'
        if program:
            p = program.pop(0)
            if not program:
                v['need'], v['stable'] = True, st.script + 60      # (a press's screen change can take a moment)
            return p
        if v['need'] and screen != PLAY and st.script - v['stable'] >= 30:
            v['need'] = False
            on_screen(st, screen)
            if program:
                return program.pop(0)
        if GAMEOVER_REPLAY and screen == REPLAY:
            return replay_shots(st)
        if screen == PLAY and st.mode == 2 and v['game']:
            v['play_t'] += 1
            if v['play_t'] % 120 == 0:
                v['last_scores'] = scores(run)
            if v['game'] == 'strip' and v['play_t'] > ticks:
                log(st, 'strip: END RUN after', v['play_t'])
                v['play_t'] = -10 ** 9
                queue('START', 90, 'UP', 'CROSS', 150, 'DOWN', 'CROSS', 300)
                return program.pop(0)
            if not single and not v['paused'] and v['play_t'] == 1500 and v['game'] not in CONTESTS:
                v['paused'] = True                # the pause tour: VIEW RECORDS (RIGHT), OPTIONS (a row each way), CONTINUE
                queue('START', 90, 'DOWN', 'CROSS', 150, 'RIGHT', 60, 'TRIANGLE', 90, 'DOWN', 'CROSS', 150,
                      'RIGHT', 40, 'LEFT', 40, 'TRIANGLE', 90, 'UP', 'UP', 'CROSS', 90)
                return program.pop(0)
            if v['game'] == 'paintball' and os.environ.get('PB_SWEEP') == '1':
                # (PB_SWEEP=1) the rider stays put and the scope (the other pad's, the same presses) sweeps: a direction
                # held in turn (left, down, right, up, 40 ticks each), CIRCLE fired every 20 ticks, R1 (the next
                # projectile) every 900 ticks
                k = v['play_t']
                d = ('LEFT', 'DOWN', 'RIGHT', 'UP', 'LEFT', 'LEFT', 'DOWN', 'DOWN')[(k // 40) % 8]
                b = {d}
                if k % 20 < 3:
                    b.add('CIRCLE')
                if k % 900 < 3:
                    b = {'R1'}
                v['fired'] += k % 20 == 0
                return core.Pad(frozenset(b))
            if not st.riding or (st.pos[0] == 0.0 and st.pos[2] == 0.0):
                return core.NOTHING
            if v.get('turn_start'):
                v['turn_start'] = False
                log(st, 'turn: player %d (rider %d)' % (run.mem(CURRENT_PLAYER, 1)[0], turn_rider()))
                pl.new_turn(st)
            p = pl(st)
            if p is None:
                p = core.NOTHING
            if v['game'] == 'paintball' and v['play_t'] % FIRE_EVERY < 4:
                v['fired'] += v['play_t'] % FIRE_EVERY == 0
                p = core.Pad(p.buttons | {'CIRCLE'}, p.lx, p.ly, p.rx, p.ry)
            return p
        if screen != PLAY and st.script - max(e[0] for e in v['events']) > 1200 and st.script % 300 == 0:
            log(st, 'stuck on screen %d: CROSS' % screen)
            return core.pad('CROSS')
        return core.NOTHING

    with core.Run(cfg) as run:
        v['last_scores'] = [None, None]
        core.play(run, read=read, policy=policy, max_ticks=menu.t + 400000, recorder=rec, trace=trace)
    trace.close()
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(menu.text() + ',' + rec.text())
    stats = meter.s.json()
    stats['games'] = v['scores']
    stats['fired'] = v['fired']
    stats['screens'] = sorted(set(e[1] for e in v['events']))
    meta = dict(name=name, level=level, rights=rights, rider=0, seed=seed, ticks=ticks, games=list(games), end=run.script_time,
                end_tick=run.tick, env=env)
    core.save_json(os.path.join(d, 'stats.json'), stats)
    core.save_json(os.path.join(d, 'meta.json'), meta)
    print(name, meter.s.row(dm2.COLUMNS), 'games', v['scores'], 'fired', v['fired'], 'screens', stats['screens'], flush=True)


def main():
    a = sys.argv[1:]
    if a and a[0] == 'play':
        play_mp(a[1], seed=int(a[2]), games=a[3].split(','), ticks=int(a[4]) if len(a) > 4 else 5400,
                gs=os.environ.get('GS') == '1', max_secs=int(os.environ.get('MAX_SECS', '1500')),
                level=int(os.environ.get('LEVEL', '0')), rights=int(os.environ.get('RIGHTS', '0')))


if __name__ == '__main__':
    main()
