#!/usr/bin/env python3
"""BMX XXX's bail with the hands reaching (verzge2): bmx.py's player on rails only, made to fall off them, and the bail's
reach held while the rider tumbles, so a hand that comes near the rail's edge holds it.

    bmxbailreach.py NAME [LEVEL [SEED [TICKS]]]

The condition (games/bmx/rider/actions/bail.cpp): in the bail's tumble (physics 2, kBailTumbling) the reach move held
(kMoveBailReach, the grind button) sets kFlagBailHands; bailHands then looks for a rail edge within the hand's reach
(bailHandContact) and, once a hand holds one, bailHandsHeld sets the hand's body contact on it (setContactPoint,
enableContact) every frame of the bail. The player grinds (RAILS=1), and after FALL_AFTER frames on a rail pushes the
stick the way the balance already leans, so the grind tips over and the rider bails beside the rail; in a rail hop's
air it holds a trick into the landing (TRICK_IN_AIR), a bail where the hop came down; every frame of a bail it holds
the grind button. A played session as bmx.play_session's (sessions/NAME: script.txt, stats.json,
play.log); bails.json: each bail's start, length and frames with the reach held.
"""
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmx  # noqa: E402
import core  # noqa: E402

FALL_AFTER = int(os.environ.get('FALL_AFTER', '30'))   # frames on a rail before the balance is pushed over
REACH = os.environ.get('REACH', 'TRIANGLE')             # the bail's reach: the grind button
TRICK_IN_AIR = os.environ.get('TRICK_IN_AIR', 'CIRCLE')  # held in a rail hop's air, so the landing is a bail beside the rail
NO_HOP = os.environ.get('NO_HOP') == '1'                 # the rail's run-up without its hop: the rider rides into the rail


def rail_distance(rails, p):
    """The distance from p to the nearest rail of the level's (their polylines)."""
    best = None
    for r in rails:
        pts = r.get('pts') or []
        for a, b in zip(pts, pts[1:]):
            d = [b[i] - a[i] for i in range(3)]
            dd = sum(x * x for x in d) or 1e-9
            t = max(0.0, min(1.0, sum((p[i] - a[i]) * d[i] for i in range(3)) / dd))
            q = [a[i] + d[i] * t - p[i] for i in range(3)]
            v = sum(x * x for x in q) ** 0.5
            best = v if best is None or v < best else best
    return best


def bail_player(base_player_of):
    def player_of(level, seed, log):
        pl = base_player_of(level, seed, log)
        pl.rail_only = True
        seen = {'grind': 0, 'bail': None, 'bails': []}

        class BailReach(type(pl)):
            def __call__(self, st):
                if st.physics == bmx.BAIL:
                    if seen['bail'] is None:
                        near = rail_distance(self.rails, st.pos)
                        seen['bail'] = {'start': st.script, 'frames': 0, 'rail': near, 'nearest': near}
                        seen['bails'].append(seen['bail'])
                        self.log(st, 'bail: reach held (%s)' % REACH)
                    seen['bail']['frames'] += 1
                    if seen['bail']['frames'] % 10 == 0:
                        near = rail_distance(self.rails, st.pos)
                        if near is not None and (seen['bail']['nearest'] is None or near < seen['bail']['nearest']):
                            seen['bail']['nearest'] = near
                    return core.pad(REACH)
                seen['bail'] = None
                task = getattr(self.task, 'name', '')
                if st.physics == bmx.AIR and task == 'rail' and TRICK_IN_AIR:
                    # (a trick held into the landing: the rider bails where the rail's hop came down, beside it)
                    seen['air'] = seen.get('air', 0) + 1
                    return core.pad(TRICK_IN_AIR)
                if NO_HOP and task == 'rail':
                    out = super().__call__(st)
                    if out is not None and 'CROSS' in out.buttons:
                        out = core.Pad(frozenset(b for b in out.buttons if b != 'CROSS'), out.lx, out.ly, out.rx, out.ry)
                    return out
                if st.physics == bmx.GRIND:
                    seen['grind'] += 1
                    if seen['grind'] > FALL_AFTER:
                        return core.pad(lx=1.0 if st.balance >= 0 else -1.0)
                else:
                    seen['grind'] = 0
                return super().__call__(st)

        pl.__class__ = BailReach
        pl.bail_record = seen
        return pl
    return player_of


def main():
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    name = a[0]
    level = int(a[1]) if len(a) > 1 else 0
    seed = int(a[2]) if len(a) > 2 else 1
    ticks = int(a[3]) if len(a) > 3 else 5400
    os.environ.setdefault('RAILS', '1')
    made = []
    base = bmx.player_of

    def keep(level_, seed_, log):
        pl = bail_player(base)(level_, seed_, log)
        made.append(pl)
        return pl
    bmx.player_of = keep
    bmx.play_session(name, level=level, seed=seed, ticks=ticks, max_secs=int(os.environ.get('MAX_SECS', '900')))
    if made:
        core.save_json(os.path.join(bmx.session_dir(name), 'bails.json'), made[0].bail_record['bails'])
        print(name, 'bails', len(made[0].bail_record['bails']), 'reach frames', sum(b['frames'] for b in made[0].bail_record['bails']))


if __name__ == '__main__':
    main()
