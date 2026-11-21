#!/usr/bin/env python3
"""BMX XXX's multiplayer modes no release's SELECT GAME offers, by the collection's all-modes extra (routes2;
ZX_BMX_ALL_MP_MODES=1, game_overrides/bmx_xxx_modes.cpp): bmxmp.py's session with SELECT GAME's lines as the extra
makes them, each mode taken by its own line.

    [LEVEL=n] bmxmpmodes.py play NAME SEED GAMES [TICKS]

GAMES as bmxmp's, by the extra's names: skillz, highfive, randomspot, wipeout, grind, walltap, bigair, combo, spin,
jump, manual, nosewheelie, paintball, strip. The extra lists its modes {4, 5, 7, 8, 9 .. 18} then EXIT, and starts the
chosen line's mode itself (kOfferedModes[line]): a poke of g_mpGameSelectModes (routes1's bmxmpall.py) changes nothing
there, so its HIGH FIVE, RANDOM SPOT and WIPEOUT were all line 0, SKILLZ (mode 4), and hit none of those modes' code.
The mode the game runs is logged at each game's instructions screen (g_currentGameMode). In a mode whose players take
turns (RANDOM SPOT) the rider ridden is the turn's.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmxmp  # noqa: E402

# The extra's lines, in its order (kOfferedModes), EXIT after them.
OFFERED = ('skillz', 'highfive', 'randomspot', 'wipeout', 'grind', 'walltap', 'bigair', 'combo', 'spin', 'jump', 'manual',
           'nosewheelie', 'paintball', 'strip')

if __name__ == '__main__':
    os.environ['ZX_BMX_ALL_MP_MODES'] = '1'
    bmxmp.MODES = OFFERED
    bmxmp.SELECT_LINES, bmxmp.SELECT_EXIT = len(OFFERED) + 1, len(OFFERED)
    bmxmp.TURN_RIDER_ALWAYS = True
    bmxmp.CONTESTS.clear()      # the contests too by their own lines, no poke
    bmxmp.main()
