#!/usr/bin/env python3
"""Aggressive Inline booted with no memory card in either slot (routes1): the boot's autoload finds none and asks
(Retry, or go on without saving); the question is left up a while (the job polls the slots every round), then the
game goes on without saving to the main menu. The runner's PS2X_MC_SLOT1/2=none (MemoryCard.cpp), the card's own
jobs (PS2X_SAVES=card).

    ainocard.py NAME [WAIT]     WAIT: ticks the question stays up (900)

Closed on the game's state: the intro's START presses until a message box (MODE 3, routes.py's box mode) is up, then
on each box: the first answered after WAIT with DOWN, CROSS (the second choice), any later one with CROSS; the run ends
600 ticks after the main menu is up (0x276FE4 == 1 and the front end's panel, routes.py boot's test). play.log: each
state change with its tick.

(routes1) As run: the question came up and stayed (MODE never went to 3, so no box was answered) to the tick limit
(12,000): the autoload's no-card path, findMemoryCard polled every round (200 calls kept), the required-space text
(appendRequiredSpaceText, appendClientRequiredSpace). Answering it (the main menu after) needs the question's own
state, not MODE.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ai  # noqa: E402
import core  # noqa: E402

BOX_MODE = 3
MAIN_MENU_FLAG, FRONT_END_PANEL = 0x276FE4, 0x240A20


def play(name, wait=900, ticks=12000):
    env = {'PS2X_MC_SLOT1': 'none', 'PS2X_MC_SLOT2': 'none'}
    cfg = ai.config(name, env=env, secs=1500)
    d = os.path.join(ai.SCRATCH, 'sessions', name)
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, 'play.log'), 'w')
    s = {'boxes': 0, 'prog': [], 'last': None}

    def policy(st):
        menu = run.u32(MAIN_MENU_FLAG) == 1 and run.u32(FRONT_END_PANEL) != 0
        key = (st.mode, st.panel, menu)
        if key != s['last']:
            s['last'] = key
            log.write('%d mode %d panel %#x main menu %d boxes %d\n' % (st.tick, st.mode, st.panel, menu, s['boxes']))
        if s['prog']:
            return s['prog'].pop(0)
        if menu and s['boxes']:
            s['menu'] = s.get('menu', 0) + 1
            return None if s['menu'] > 600 else core.NOTHING
        if st.mode == BOX_MODE:
            s['boxes'] += 1
            first = s['boxes'] == 1
            s['prog'] = [core.NOTHING] * (wait if first else 200) + ([core.pad('DOWN')] * 6 + [core.NOTHING] * 30 if first else []) \
                + [core.pad('CROSS')] * 6 + [core.NOTHING] * 120
            return s['prog'].pop(0)
        if st.tick > 400 and st.tick % 120 < 6 and not menu:
            return core.pad('START')
        return core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, ai.read, policy, max_ticks=ticks)
    log.write('end: boxes %d, main menu reached %s\n' % (s['boxes'], bool(s.get('menu'))))
    log.close()
    print(name, 'boxes answered', s['boxes'], 'main menu', bool(s.get('menu')))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]) if len(a) > 1 else 900)
