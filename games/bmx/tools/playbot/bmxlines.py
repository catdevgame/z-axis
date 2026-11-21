#!/usr/bin/env python3
"""BMX XXX: a played run with the menu lines the game's builders leave out (routes1; ZX_BMX_MENU_LINES=1, [extras]
menu_lines, game_overrides/bmx_xxx_exit.cpp): bmx.py's player rides the level, the pause's TRICK LIST is opened now
and then (its tricks and categories walked), and at the end the run is ended from the pause and the game over's VIEW
REPLAY watched, then its TRICK LIST.

    bmxlines.py NAME LEVEL [TICKS [SEED]]

The menus, as the extra builds them (each line at its choice's place): PAUSED: CONTINUE, VIEW CHALLENGES, VIEW BIKE
PARTS, VIEW RECORDS, OPTIONS, TRICK LIST, END RUN; GAME OVER (a game of the tour): ANOTHER RUN, CHANGE RIDER, SAVE
GAME, VIEW REPLAY, TRICK LIST, VIEW RECORDS, EXIT TO MAIN MENU. Every screen change is logged (g_gameScreen) in
sessions/NAME/play.log, with the run's numbers in stats.json (dm2.Meter's columns); GS=1: a picture at each.
TRICK_LIST_EVERY=N: the pause visits' spacing (2400).

(routes1) The pause's TRICK LIST visit records every function of the trick list screen. The end does not reach the
game over yet: in the tour's challenge (MULY) the pause's last line with START, UP, CROSS, DOWN, CROSS starts the run
again (the CHALLENGES list, screen 26) instead of ending it, so VIEW REPLAY is not reached.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import core  # noqa: E402
import bmx  # noqa: E402

presses = bmx.presses
TRICK_LIST_EVERY = int(os.environ.get('TRICK_LIST_EVERY', '2400'))   # ticks of riding between the pause's TRICK LIST visits
PAUSED, SCREEN_PAUSED = 3, 7   # bmx.MODE's pause, g_gameScreen's PAUSED


def pause_trick_list():
    """PAUSED, TRICK LIST (the sixth line), its list walked (DOWN, UP, a category opened with CROSS, its tricks walked,
    TRIANGLE back); the way back to riding is closed on the screen (unpause)."""
    return presses('START', 60, *['DOWN'] * 5, 'CROSS', 150, 'DOWN', 'DOWN', 'DOWN', 'UP', 'CROSS', 120, 'DOWN', 'DOWN',
                   'RIGHT', 'LEFT', 'TRIANGLE', 90, 'DOWN', 'CROSS', 120, 'DOWN', 'TRIANGLE', 90)


def game_over_choice(downs, then=()):
    return presses(120, *['DOWN'] * downs, 'CROSS', 200, *then)


def play(name, level, ticks=4800, seed=1, max_secs=2400):
    import dm2
    import bmxmap  # noqa: F401  (the BMX scratch's maps for dm2map, as bmx.play_session)
    import dm2map
    import player_dm2
    menu = bmx.menu_script(level)
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    env = {'ZX_EE_SCALE': '4096', 'ZX_BMX_MENU_LINES': '1'}
    cfg = bmx.run_config(name, menu.text(), env=env, max_secs=max_secs, gs=os.environ.get('GS') == '1')
    meter, log = dm2.Meter(), player_dm2.Log(os.path.join(d, 'play.log'))
    pl = bmx.player_of(dm2map.Level(bmx.LEVELS[level][0]), seed, log)
    rec = core.Recorder()
    s = {'program': [], 'visits': 0, 'lists': 0, 'replay_frames': 0}

    def policy(st):
        meter.tick(st)
        screen = st.screen if st.frame else -1
        if (screen, st.mode) != s.get('screen'):
            s['screen'] = (screen, st.mode)
            log(st, 'screen %d mode %d riding %d' % (screen, st.mode, st.riding))
            if os.environ.get('GS') == '1':
                s['shots'] = s.get('shots', []) + [(st.script + 30, 's%d-%d-%d' % (st.script, screen, st.mode))]
        for at, what in [x for x in s.get('shots', []) if x[0] <= st.script]:
            s['shots'].remove((at, what))
            log(st, 'shot', run.shot(what))
        if s['program']:
            return s['program'].pop(0)
        if s.get('ending'):
            if screen == bmx.SCREEN_GAME_OVER and not s.get('at_over'):
                s['at_over'] = True
                s['visits'] += 1
                log(st, 'game over, visit %d' % s['visits'])
                if s['visits'] == 1:                    # VIEW REPLAY
                    s['program'] = game_over_choice(3)
                elif s['visits'] == 2:                  # TRICK LIST, walked, back
                    s['program'] = game_over_choice(4, ('DOWN', 'DOWN', 'UP', 'CROSS', 120, 'DOWN', 'TRIANGLE', 90,
                                                        'TRIANGLE', 120))
                else:
                    return None
                return s['program'].pop(0)
            if screen != bmx.SCREEN_GAME_OVER:
                s['at_over'] = False
                if s['visits'] == 1:
                    s['replay_frames'] += 1
                s['wait'] = s.get('wait', 0) + 1
                if s['wait'] > 6000:
                    log(st, 'no game over screen again')
                    return None
                # the run's summaries before the game over (CONTINUE); a replay plays to its end by itself
                return core.pad('CROSS') if s['visits'] == 0 and st.script % 120 < 4 else core.NOTHING
            s['wait'] = 0
            return core.NOTHING
        if st.mode == PAUSED and s.get('t') is not None:
            # back from a pause visit: TRIANGLE until PAUSED (screen 7) is up, then START to ride on
            k = s['unpause'] = s.get('unpause', 0) + 1
            if k % 90 == 0:
                return core.pad('START') if screen == SCREEN_PAUSED else core.pad('TRIANGLE')
            return core.NOTHING
        s['unpause'] = 0
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
        if t > ticks:
            s['ending'] = True
            log(st, 'end run')
            s['program'] = presses(120, 'START', 60, 'UP', 'CROSS', 150, 'DOWN', 'CROSS', 200)   # (a START just after a pause is not seen)
            return s['program'].pop(0)
        if t >= TRICK_LIST_EVERY * (s['lists'] + 1):
            s['lists'] += 1
            log(st, 'pause trick list %d' % s['lists'])
            s['program'] = pause_trick_list()
            return s['program'].pop(0)
        return pl(st)

    with core.Run(cfg) as run:
        pl.run = run
        core.play(run, bmx.read, policy, max_ticks=ticks + menu.t + 30000, recorder=rec)
    stats = meter.s.json()
    stats.update(trick_lists=s['lists'], game_over_visits=s['visits'], replay_frames=s['replay_frames'], areas=len(pl.seen))
    core.save_json(os.path.join(d, 'stats.json'), stats)
    with open(os.path.join(d, 'script.txt'), 'w') as f:
        f.write(menu.text() + ',' + rec.text())
    print(name, meter.s.row(dm2.COLUMNS), 'trick lists', s['lists'], 'game over visits', s['visits'],
          'replay frames', s['replay_frames'])


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) < 2:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]), int(a[2]) if len(a) > 2 else 4800, int(a[3]) if len(a) > 3 else 1)
