#!/usr/bin/env python3
"""Aggressive Inline's boot for the functions that never return (routes2, fncheck_stops.h): the boot movies played
through (the movie player's default thread, movieDefaultMain), then the memory card's own jobs (ZX_SAVES=card:
the boot's autoload job, each ending in endSaveJob, which deletes the job's thread) on the card in slot 1, then the
main menu. Record it with ZX_FNREC_STOPS=1 and the stop table's functions (routes2 bin/recstop.sh).

    aistops.py NAME [MOVIE_TICKS [TICKS [hle]]]   MOVIE_TICKS: ticks before the first START (2400: the intros play
                                                   out); hle: the movies on the PS2 player (ZX_MOVIES=hle, its
                                                   default thread movieDefaultMain; the host decodes them otherwise)

(routes2) With hle the first intro (AKA) stalls at vsync 48 in an unpaced muted run (the HLE's demux held for
backpressure behind the game's audio ring, MPEG.cpp traceMovieStalls), its default thread switching for good: one
movieDefaultMain call, stopped a vsync in. Without hle the boot reaches the card jobs.

Closed on the game's state: no input while the movies play, then START every two seconds until the main menu is up
(0x276FE4 == 1 and the front end's panel, routes.py boot's test), message boxes (MODE 3) answered with CROSS; the
run ends 600 ticks after the main menu. play.log: each state change with its tick.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ai  # noqa: E402
import core  # noqa: E402

BOX_MODE = 3
MAIN_MENU_FLAG, FRONT_END_PANEL = 0x276FE4, 0x240A20


def play(name, movie_ticks=2400, ticks=9000, hle=False):
    env = {'ZX_SAVES': 'card'}
    if hle:
        env['ZX_MOVIES'] = 'hle'
    cfg = ai.config(name, env=env, secs=1500)
    d = os.path.join(ai.SCRATCH, 'sessions', name)
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, 'play.log'), 'w')
    s = {'boxes': 0, 'menu': 0, 'last': None, 'prog': []}

    def policy(st):
        menu = run.u32(MAIN_MENU_FLAG) == 1 and run.u32(FRONT_END_PANEL) != 0
        key = (st.mode, st.panel, menu)
        if key != s['last']:
            s['last'] = key
            log.write('%d mode %d panel %#x main menu %d boxes %d\n' % (st.tick, st.mode, st.panel, menu, s['boxes']))
        if s['prog']:
            return s['prog'].pop(0)
        if menu:
            s['menu'] += 1
            return None if s['menu'] > 600 else core.NOTHING
        if st.mode == BOX_MODE:
            s['boxes'] += 1
            s['prog'] = [core.pad('CROSS')] * 6 + [core.NOTHING] * 120
            return s['prog'].pop(0)
        if st.tick > movie_ticks and st.tick % 120 < 6:
            return core.pad('START')
        return core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, ai.read, policy, max_ticks=ticks)
    log.write('end: boxes %d, main menu reached %s\n' % (s['boxes'], bool(s['menu'])))
    log.close()
    print(name, 'boxes answered', s['boxes'], 'main menu', bool(s['menu']))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]) if len(a) > 1 else 2400, int(a[2]) if len(a) > 2 else 9000, len(a) > 3 and a[3] == 'hle')
