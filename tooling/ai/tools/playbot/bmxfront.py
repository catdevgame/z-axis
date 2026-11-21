#!/usr/bin/env python3
"""BMX XXX booted with no memory card (routes1).

    bmxfront.py nocard NAME        booted with no memory card in either slot (ZX_MC_SLOT1/2=none) and the game's own
                                   card jobs (ZX_SAVES=card): the boot's autoload finds none and its question stays
                                   up, polling the slots, to the tick limit (12,000)

play.log in sessions/NAME: each mode change and step with its tick.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmx  # noqa: E402
import core  # noqa: E402


def play(kind, name, ticks=12000):
    env = {'ZX_EE_SCALE': '4096'}
    if kind == 'nocard':
        env.update(ZX_SAVES='card', ZX_MC_SLOT1='none', ZX_MC_SLOT2='none')
    cfg = bmx.run_config(name, '', env=env, max_secs=1500)
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, 'play.log'), 'w')
    s = {'mode': None}

    def policy(st):
        if st.mode != s['mode']:
            s['mode'] = st.mode
            log.write('%d mode %d\n' % (st.tick, st.mode))
        if kind == 'nocard':
            # the title's START twice; the card question is left up
            return core.pad('START') if any(t <= st.tick < t + 6 for t in (1700, 2300)) else core.NOTHING
        return core.NOTHING

    with core.Run(cfg) as run:
        core.play(run, bmx.read, policy, max_ticks=ticks)
    log.write('end at tick %d\n' % run.tick)
    log.close()
    print(name, kind, 'end tick', run.tick)


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) < 2 or a[0] != 'nocard':
        raise SystemExit(__doc__)
    play(a[0], a[1])
