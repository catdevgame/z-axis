#!/usr/bin/env python3
"""The rides of BMX XXX's logic check: played closed loop natively, kept as a schedule by level frame.

A ride is a list of steps (ride to a point, hold buttons, brake); the player reads the rider each pad read
(the play link, tools/playbot core.py and bmx.py's table) and answers with buttons only, changed on even
level frames (so two changes are >= 2 frames apart, as bmxlogic.py's frame anchors need). What it pressed is
written as "FRAME BUTTONS" lines: tools/scripts/bmx-ref-run1-schedule.txt is RIDES['run1'], which PCSX2 then
replays open loop (bmxpcsx2.py `schedule`, the hook applies it by level frame) for the reference. The player
is deterministic: the same runner and steps give the same schedule.

    python3 -I tools/bmxride.py play NAME [RIDE]   -> $BMXRIDE_DIR/NAME/schedule.txt and log.txt

Environment: RUNNER (a runner with BMX XXX), ELF, BMXRIDE_DIR (default $ZAXIS_SCRATCH/bmxride),
DUMP_AT (level frames to keep RAM images at), VERBOSE.
"""
import math
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(ROOT)), 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402

zpaths.use('playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'dm2', 'bmx')   # (the playbot's core and the bikes' players)
import core  # noqa: E402
import bmx  # noqa: E402

OUT = os.environ.get('BMXRIDE_DIR', os.path.join(zpaths.ZAXIS_SCRATCH, 'bmxride'))


def heading(st):
    return math.atan2(st.fwd[2], st.fwd[0])


def wrap(a):
    return (a + math.pi) % (2 * math.pi) - math.pi


def at(frame, buttons=(), hold=6):
    """Press buttons at a level frame for `hold` frames."""
    def step(st, t):
        if st.frame < frame:
            return frozenset()
        return frozenset(buttons) if st.frame < frame + hold else None
    step.__name__ = 'at%d:%s' % (frame, '+'.join(buttons))
    return step


def hold(buttons, frames):
    def step(st, t):
        return frozenset(buttons) if t < frames else None
    step.__name__ = 'hold:%s:%d' % ('+'.join(buttons), frames)
    return step


def goto(x, z, near=2.0, limit=1500, lead=6.0, band=0.06):
    """Ride to (x, z) pedalling: LEFT or RIGHT on the heading error predicted `lead` frames on (a turn carries on
    after its button is let go); braking when the point is inside the turn; CROSS when held still (after a bail)."""
    state = {}

    def step(st, t):
        dx, dz = x - st.pos[0], z - st.pos[2]
        d = math.hypot(dx, dz)
        if d < near or t > limit:
            if t > limit:
                print('goto %.1f %.1f: limit, at %.1f %.1f' % (x, z, st.pos[0], st.pos[2]))
            return None
        h = heading(st)
        w = wrap(h - state.get('h', h)) / max(1, st.frame - state.get('f', st.frame - 1))
        state['h'], state['f'] = h, st.frame
        err = wrap(math.atan2(dz, dx) - h)
        pred = err - w * lead
        b = set()
        state['still'] = state.get('still', 0) + 1 if st.speed < 0.01 else 0
        if state['still'] > 60 and state['still'] % 60 < 4:
            return frozenset(['CROSS'])
        if abs(err) > 0.9 and d < 12:
            if st.speed > 0.12:
                b.add('DOWN')
        else:
            b.add('UP')
        if pred > band:
            b.add('LEFT')
        elif pred < -band:
            b.add('RIGHT')
        return frozenset(b)
    step.__name__ = 'goto(%.1f,%.1f)' % (x, z)
    return step


def stop(limit=200):
    def step(st, t):
        return None if st.speed < 0.005 and t > 4 or t > limit else frozenset(['DOWN'])
    return step


def run1():
    """The Bronx (rider 0): the overview and CHALLENGES, down the street from the start and off the kerb, a hop
    onto the low rail (x 12.5, z 30..35) and a grind, Homeless Jones (7.15, 74.08) by TRIANGLE and his lines (his
    challenge: cans), back south on x 5 through cans 04, 09 and 10, a hop with a trick (SQUARE+LEFT), a stop."""
    return [
        at(200, ['CROSS']), at(320, ['CROSS']), at(420, [], 0),
        goto(14.0, -38.3, near=3.0), goto(12.5, 0.0, near=3.0), goto(12.5, 24.0, near=1.5),
        hold(['UP', 'CROSS'], 8), hold(['TRIANGLE'], 40), hold([], 50),
        goto(9.0, 66.0, near=1.5), stop(), hold([], 30),
        hold(['TRIANGLE'], 6), hold([], 94),
        hold(['CROSS'], 6), hold([], 94), hold(['CROSS'], 6), hold([], 94),
        hold(['CROSS'], 6), hold([], 94), hold(['CROSS'], 6), hold([], 60),
        goto(5.2, 64.0, near=1.5), goto(5.25, 60.0, near=1.0), goto(5.3, 29.45, near=1.0), goto(5.25, 12.0, near=1.0),
        hold(['UP', 'CROSS'], 10), hold([], 4), hold(['SQUARE', 'LEFT'], 10), hold([], 50),
        goto(5.0, -40.3, near=1.0), goto(5.0, -50.0, near=1.0), stop(), hold([], 200),
    ]


RIDES = {'run1': (run1, 0, 0)}   # name: (steps, level, rider)


def play(name, ride):
    steps, level, rider = RIDES[ride]
    steps = steps()
    cur = {'step': None, 't0': 0, 'b': frozenset()}
    menu = bmx.menu_script(level, rider)
    cfg = bmx.run_config(name, menu.text(), env={'ZX_PAD_STICK_CENTER': '0x7f'}, max_secs=900, root=OUT)
    d = os.path.join(OUT, name)
    os.makedirs(d, exist_ok=True)
    sched, log = [], open(os.path.join(d, 'log.txt'), 'w')
    dump_at = [int(x) for x in os.environ.get('DUMP_AT', '').split(',') if x]
    seen = {}

    def next_buttons(st):
        while steps:
            if cur['step'] is not steps[0]:
                cur['step'], cur['t0'] = steps[0], st.frame
                log.write('f%d start %s\n' % (st.frame, steps[0].__name__))
            out = steps[0](st, st.frame - cur['t0'])
            if out is not None:
                return out
            steps.pop(0)
        return None

    def policy(st):
        if st.frame == 0 or st.mode not in (2, 3):
            return core.NOTHING
        if st.frame % 2 == 0:
            b = next_buttons(st)
            if b is None:
                return None
            cur['b'] = b
        b = cur['b']
        if not sched or sched[-1][1] != b:
            sched.append((st.frame, b))
        if st.frame in dump_at:
            dump_at.remove(st.frame)
            with open(os.path.join(d, 'f%d.ram' % st.frame), 'wb') as f:
                f.write(run.mem(0, 0x2000000))
        ev = (st.physics, st.trick != 0xFF, st.line_tricks, int(st.run_score), st.action)
        if st.frame % 30 == 0 or ev != seen.get('ev'):
            seen['ev'] = ev
            line = ('f%5d mode %d ph %2d act %2d trick %3d pos %7.2f %7.2f %7.2f sp %.3f hd %4.0f score %6.0f line %d/%d %s'
                    % (st.frame, st.mode, st.physics, st.action, st.trick, *st.pos, st.speed, math.degrees(heading(st)),
                       st.run_score, st.line_score, st.line_tricks, '+'.join(sorted(b)) or '-'))
            log.write(line + '\n')
            if os.environ.get('VERBOSE'):
                print(line, flush=True)
        return core.Pad(b)

    run = core.Run(cfg)
    with run:
        core.play(run, bmx.read, policy, max_ticks=menu.t + 60000)
    with open(os.path.join(d, 'schedule.txt'), 'w') as f:
        for fr, b in sched:
            f.write('%d %s\n' % (fr, '+'.join(sorted(b)) or '-'))
    print('%s: %d changes, %s' % (name, len(sched), os.path.join(d, 'schedule.txt')))


def main():
    a = sys.argv[1:]
    if a[:1] == ['play'] and len(a) > 1:
        play(a[1], a[2] if len(a) > 2 else 'run1')
    else:
        print(__doc__)


if __name__ == '__main__':
    main()
