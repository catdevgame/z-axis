#!/usr/bin/env python3
"""DM2 ride 10 for ridercov (ride 9 plus patterns 23-28: the air trick's spin kinds 1 and 5 and every
tap sequence (with PS2X_DEV_UNLOCK_ALL=1 every trick is open), the wall ride's crouch and hop off
(airEnter from a wall ride), long manuals (groundMove's sliding back wheel)):
 23  speed, a hop, one quick stick tap down in the air (backflip, kind 1), then SQUARE or CIRCLE
 24  speed, a hop, a tap left / right / up then down (the backflip combinations, kind 1), a trick
 25  speed, a hop, up up up (double front flip, kind 5; locked unless the tricks are open), a trick
 26  speed, a hop, one of the 21 riders' tap sequences (three taps, diagonals as two axes), a trick
 27  ride 8's wall ride (two quick taps down or up in the air, the stick held through the landing),
     then CROSS held 6-16 vsyncs and let go on the wall (the wall's hop: air from a wall ride)
 28  pedal, the stick pulled back for a long manual (240 vsyncs), a shoulder button held in it
DM2 ride 9 for rider5c (ride 8 plus patterns 17-22: the air trick's spin kinds, grinds and stalls from
the ground while coasting, the fakie's hop, grind tricks to the rail's end, bails on rails)
ride 8 for rider5b (ride 7 aimed at wall rides and grinds off: patterns 15, 16, 9, 10, 12)
ride 6 for rider5's recordings (round 5: action states, grinds, stalls, lips, wall rides,
bails) on any level: dm2logic's reference input to the level select, MENU presses of RIGHT there
(the menu value: 0 Woodward Camp = level 2, 1 = level 1, 2 = level 0, 3 = 3, 4 = 6, 5 = 5, 6 = 4,
7 = 7; the others need PS2X_DEV_UNLOCK_ALL=1 or PS2X_DM2_OPEN_LEVELS=1), then CYCLES x 300 vsyncs,
each heading another way, of patterns picked by a seeded generator:
  0  a hop with nothing held, CROSS in the air (stall / lip ask with no taps)
  1  speed, a hop, trick buttons over and over with a direction until landing (bails)
  2  speed, a hop, CROSS in the air with a stick direction (lip and stall approaches)
  3  speed, a hop, TRIANGLE in the air twice (grind asks)
  4  a long straight pedal, a hop at speed, TRIANGLE twice (rails further away)
  5  a long pedal, CIRCLE then SQUARE held near the end (wall ride and stall asks at a wall)
  6  pedal, braking held long (rolling backwards: fakie), then a turn round
  7  pedal, the shoulder buttons held on the ground (the manual), a hop out of it
  8  a long pedal, a hop, the stick held up or down with a trick (flips; bails), TRIANGLE late
  9  a pedal with the grind button held on the ground (a grind from riding), CROSS held and let go
     on the rail (the grind's crouch and hop)
 10  speed, a hop, TRIANGLE twice, then the stick held to one side (the grind's balance: off or a bail)
 11  pedal, the stick pulled back (the manual), each shoulder button held in turn (the manual's turn)
 12  speed, a hop, CROSS in the air (a stall), CROSS held and let go on it, or nothing (its end)
 13  a long pedal, all four shoulder buttons in the air near the end (a wall ride), the stick up or
     down held on the wall (the balance), late CROSS held and let go (the wall's hop)
 14  a long straight pedal with no hop (into whatever is ahead: bails from the ground)
 15  a long pedal, a hop, two quick stick taps down (or up) in the air, the stick held that way
     through the landing (airState's wall ride ask: physics 10 or 9), then the stick the other way
 16  speed, a hop, TRIANGLE twice (a grind), then CROSS held 20 and let go (the grind's hop off), or
     nothing until it ends (off the rail's end)
 17  speed, a hop, quick stick taps in the air (down down, up up, down down down or up up up: the
     spin kinds 2 .. 5), then SQUARE or CIRCLE
 18  a pedal, then coasting (nothing held) with TRIANGLE held (a grind or stall from the ground on
     a ledge's edge), steering a little
 19  pedal, braking held (fakie), the stick to one side (the turn round) with a hop in it
 20  speed, a hop, TRIANGLE twice (a grind), trick buttons on the rail, then nothing (to its end)
 21  a slow coast with TRIANGLE held, then CROSS held and let go (a stall from the ground and off it)
 22  speed, a hop, TRIANGLE twice, then trick buttons and the stick over and over (bails on the rail)
"""""
import random

# The new patterns three times as often as ride 6's.
WEIGHTS = [23] * 5 + [24] * 5 + [25] * 4 + [26] * 8 + [27] * 6 + [28] * 3 + [17] * 2 + [18, 20, 21, 22, 9, 12, 15]
ALL_PATTERNS = list(range(29))
# The tap sequences (0x2172B0: 21 groups of four; the used ones' taps), directions 1 .. 8.
SEQS = [(6, 2, 1), (4, 4, 4), (2, 2, 2), (2, 1, 3), (8, 3, 8), (8, 8, 8), (2, 1, 4), (2, 1, 2), (8, 4, 8), (1, 2, 1),
        (2, 2, 3), (1, 3, 2), (3, 6, 2), (4, 3, 4), (3, 3, 3), (3, 4, 3), (5, 3, 6)]
# Direction -> (LX, LY): 1 left, 2 right, 3 up, 4 down, 5 up-left, 6 up-right, 7 down-left, 8 down-right.
AXES = {1: (-0.95, 0), 2: (0.95, 0), 3: (0, -0.95), 4: (0, 0.95), 5: (-0.95, -0.95), 6: (0.95, -0.95),
        7: (-0.95, 0.95), 8: (0.95, 0.95)}


def taps(items, at, dirs, hold=3, gap=4):
    for j, d in enumerate(dirs):
        x, y = AXES[d]
        t = at + j * (hold + gap)
        if x:
            items.append('%dv:LX=%g:%dv' % (t, x, hold))
        if y:
            items.append('%dv:LY=%g:%dv' % (t, y, hold))
    return at + len(dirs) * (hold + gap)


def ride(t0, cycles, seed, only=None):
    """The pad script items of CYCLES x 300 vsyncs of patterns from script time t0."""
    rng = random.Random(seed)
    items = []
    tricks = ['SQUARE', 'CIRCLE', 'SQUARE', 'R1', 'CIRCLE', 'L1']
    for k in range(cycles):
        b = t0 + 300 * k
        turn = rng.randint(4, 60)
        items.append('%dv:LX=%g:%dv' % (b, rng.choice((-0.9, 0.9)), turn))
        s = b + turn + 4
        p = rng.choice(only or WEIGHTS)
        if p == 0:
            items.append('%dv:UP:30v' % s)
            items.append('%dv:CROSS:24v' % (s + 40))
            items.append('%dv:CROSS:4v' % (s + 74))
            items.append('%dv:CROSS:4v' % (s + 84))
        elif p == 1:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            dx = rng.choice((-0.9, 0.9))
            for j, t in enumerate(range(s + 136, s + 226, 8)):
                items.append('%dv:LX=%g:6v' % (t, dx))
                items.append('%dv:%s:5v' % (t + 1, tricks[(k + j) % len(tricks)]))
        elif p == 2:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            sx, sy = rng.choice([(-0.9, 0), (0.9, 0), (0, -0.9), (0, 0.9)])
            t = s + 140
            if sx:
                items.append('%dv:LX=%g:16v' % (t, sx))
            if sy:
                items.append('%dv:LY=%g:16v' % (t, sy))
            items.append('%dv:CROSS:5v' % (t + 4))
        elif p == 3:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:TRIANGLE:5v' % (s + 140))
            items.append('%dv:TRIANGLE:5v' % (s + 156))
        elif p == 4:
            items.append('%dv:UP:230v' % s)
            items.append('%dv:CROSS:16v' % (s + 200))
            items.append('%dv:TRIANGLE:6v' % (s + 222))
            items.append('%dv:TRIANGLE:6v' % (s + 240))
        elif p == 5:
            items.append('%dv:UP:230v' % s)
            items.append('%dv:CIRCLE:20v' % (s + 190))
            items.append('%dv:CROSS:12v' % (s + 200))
            items.append('%dv:SQUARE:20v' % (s + 216))
        elif p == 6:
            items.append('%dv:UP:60v' % s)
            items.append('%dv:DOWN:150v' % (s + 70))
            items.append('%dv:LX=%g:30v' % (s + 230, rng.choice((-0.9, 0.9))))
        elif p == 7:
            items.append('%dv:UP:120v' % s)
            sh = rng.choice(('L1', 'R1', 'L2', 'R2'))
            items.append('%dv:%s:80v' % (s + 60, sh))
            items.append('%dv:LY=%g:40v' % (s + 80, rng.choice((-0.9, 0.9))))
            items.append('%dv:CROSS:16v' % (s + 150))
        elif p == 9:
            items.append('%dv:UP:220v' % s)
            items.append('%dv:TRIANGLE:150v' % (s + 40))
            items.append('%dv:CROSS:24v' % (s + 200))
        elif p == 10:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:TRIANGLE:5v' % (s + 140))
            items.append('%dv:TRIANGLE:5v' % (s + 156))
            items.append('%dv:LX=%g:100v' % (s + 170, rng.choice((-1.0, 1.0))))
        elif p == 11:
            items.append('%dv:UP:60v' % s)
            items.append('%dv:LY=0.98:200v' % (s + 70))
            for j, sh in enumerate(rng.sample(['L1', 'R1', 'L2', 'R2'], 4)):
                items.append('%dv:%s:30v' % (s + 100 + 40 * j, sh))
        elif p == 12:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:CROSS:5v' % (s + 140))
            if rng.random() < 0.6:
                items.append('%dv:CROSS:%dv' % (s + 175, rng.randint(10, 40)))
        elif p == 13:
            items.append('%dv:UP:230v' % s)
            items.append('%dv:CROSS:16v' % (s + 190))
            for sh in ('L1', 'R1', 'L2', 'R2'):
                items.append('%dv:%s:12v' % (s + 214, sh))
            items.append('%dv:LY=%g:40v' % (s + 232, rng.choice((-0.9, 0.9))))
            items.append('%dv:CROSS:20v' % (s + 262))
        elif p == 15:
            items.append('%dv:UP:230v' % s)
            items.append('%dv:CROSS:20v' % (s + 200))
            d = rng.choice((0.95, -0.95))
            items.append('%dv:LY=%g:4v' % (s + 226, d))
            items.append('%dv:LY=%g:4v' % (s + 236, d))
            items.append('%dv:LY=%g:40v' % (s + 246, d))
            items.append('%dv:LY=%g:20v' % (s + 288, -d))
        elif p == 16:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:TRIANGLE:5v' % (s + 140))
            items.append('%dv:TRIANGLE:5v' % (s + 156))
            if rng.random() < 0.6:
                items.append('%dv:CROSS:20v' % (s + 190))
        elif p == 17:
            items.append('%dv:UP:200v' % s)
            items.append('%dv:CROSS:16v' % (s + 180))
            d = rng.choice((0.95, -0.95))
            n = rng.choice((2, 3))
            for j in range(n):
                items.append('%dv:LY=%g:3v' % (s + 202 + 7 * j, d))
            items.append('%dv:%s:6v' % (s + 204 + 7 * n, rng.choice(('SQUARE', 'CIRCLE'))))
        elif p == 18:
            items.append('%dv:UP:%dv' % (s, rng.randint(80, 200)))
            items.append('%dv:TRIANGLE:120v' % (s + 120))
            items.append('%dv:LX=%g:60v' % (s + 140, rng.choice((-0.4, 0.4))))
        elif p == 19:
            items.append('%dv:UP:60v' % s)
            items.append('%dv:DOWN:150v' % (s + 70))
            items.append('%dv:LX=%g:30v' % (s + 230, rng.choice((-0.9, 0.9))))
            items.append('%dv:CROSS:12v' % (s + 236 + rng.randint(0, 14)))
        elif p == 20:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:TRIANGLE:5v' % (s + 140))
            items.append('%dv:TRIANGLE:5v' % (s + 156))
            for j in range(3):
                items.append('%dv:LX=%g:6v' % (s + 176 + 14 * j, rng.choice((-0.9, 0.9, 0))))
                items.append('%dv:%s:5v' % (s + 177 + 14 * j, rng.choice(tricks)))
        elif p == 21:
            items.append('%dv:UP:%dv' % (s, rng.randint(30, 70)))
            items.append('%dv:TRIANGLE:100v' % (s + 80))
            items.append('%dv:CROSS:%dv' % (s + 190, rng.randint(10, 40)))
        elif p == 22:
            items.append('%dv:UP:120v' % s)
            items.append('%dv:CROSS:20v' % (s + 110))
            items.append('%dv:TRIANGLE:5v' % (s + 140))
            items.append('%dv:TRIANGLE:5v' % (s + 156))
            dx = rng.choice((-0.9, 0.9))
            for j, t in enumerate(range(s + 166, s + 250, 7)):
                items.append('%dv:LX=%g:5v' % (t, dx))
                items.append('%dv:%s:4v' % (t + 1, tricks[(k + j) % len(tricks)]))
        elif p in (23, 24, 25, 26):
            items.append('%dv:UP:200v' % s)
            items.append('%dv:CROSS:16v' % (s + 180))
            if p == 23:
                dirs = [4]
            elif p == 24:
                dirs = [rng.choice((1, 2, 3, 5, 6, 7, 8)), 4]
            elif p == 25:
                dirs = [3, 3, 3]
            else:
                dirs = list(rng.choice(SEQS))
            t = taps(items, s + 200, dirs)
            items.append('%dv:%s:%dv' % (t, rng.choice(('SQUARE', 'CIRCLE')), rng.choice((6, 30))))
        elif p == 27:
            items.append('%dv:UP:230v' % s)
            items.append('%dv:CROSS:20v' % (s + 200))
            d = rng.choice((0.95, -0.95))
            items.append('%dv:LY=%g:4v' % (s + 226, d))
            items.append('%dv:LY=%g:4v' % (s + 236, d))
            items.append('%dv:LY=%g:40v' % (s + 246, d))
            items.append('%dv:CROSS:%dv' % (s + rng.randint(252, 280), rng.randint(6, 16)))
        elif p == 28:
            items.append('%dv:UP:60v' % s)
            items.append('%dv:LY=0.98:240v' % (s + 50))
            items.append('%dv:%s:60v' % (s + 120, rng.choice(('L1', 'R1', 'L2', 'R2'))))
        elif p == 14:
            items.append('%dv:UP:290v' % s)
        else:
            items.append('%dv:UP:200v' % s)
            items.append('%dv:CROSS:16v' % (s + 180))
            items.append('%dv:LY=%g:40v' % (s + 200, rng.choice((-0.9, 0.9))))
            items.append('%dv:%s:5v' % (s + 210, rng.choice(tricks)))
            items.append('%dv:TRIANGLE:5v' % (s + 250))
    return items
