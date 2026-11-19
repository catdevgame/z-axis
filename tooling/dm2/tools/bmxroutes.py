#!/usr/bin/env python3
"""Scripted routes through BMX XXX (SLUS-20415) for recording and coverage: dm2routes.py's method and
client over BMX's anchors and menus.

    bmxroutes.py start NAME [KEY=VAL ...]   launch RUNNER with a scratch HOME, card, support folder under
                                            $ROUTES_DIR/NAME
    bmxroutes.py NAME CMD [ARGS] [; CMD ...]
    bmxroutes.py stop NAME
    bmxroutes.py routes | script ROUTE

Environment: RUNNER, ROUTES_DIR (default $ZAXIS_SCRATCH/bmxcorpus/routes), ELF.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import dm2routes as base  # noqa: E402

base.ELF = os.environ.get('ELF', os.path.join(base.BMX_DATA, 'disc', 'SLUS_204.15'))
base.ROUTES_DIR = os.environ.get('ROUTES_DIR', os.path.join(base.SCRATCH, 'bmxcorpus', 'routes'))

# BMX's anchors (paired from DM2's through the work database, then watched in a run)
MODE = 0x29D040          # to be confirmed
LEVEL_FRAME = 0x2A2DE0   # rider task frames since the level started; 0 in the front end
SESSION = 0x29D040
RIDER_TABLE = 0x2F3A30
base.MODE, base.LEVEL_FRAME, base.SESSION, base.RIDER_TABLE = MODE, LEVEL_FRAME, SESSION, RIDER_TABLE
base.RIDER_POS, base.RIDER_VEL = 0x2650, 0x26C0


class Script(base.Script):
    """dm2routes' script from BMX's title: the mode turns 1 there; START, the main menu."""

    def __init__(self):
        self.items = ['1400v:SYNC=0x%X==1' % MODE]
        self.t = 1400
        self.wait(240)
        self.press('START', after=240)


# The cheats' keyboard (Extras, Cheats): 8 columns, the cursor starts on A.
KEYS = ('ABCDEFGH', 'IJKLMNOP', 'QRSTUVWX', 'YZ012345', '6789 <>')


def key_moves(text):
    """The presses that type text and press END, from A."""
    moves = []
    r, c = 0, 0
    for ch in text + '>':
        nr = next(i for i, row in enumerate(KEYS) if ch in row)
        nc = KEYS[nr].index(ch)
        if nr == 4 and c > len(KEYS[4]) - 1:      # (the last row is short)
            moves += ['LEFT'] * (c - (len(KEYS[4]) - 1))
            c = len(KEYS[4]) - 1
        moves += ['DOWN'] * max(0, nr - r) + ['UP'] * max(0, r - nr)
        moves += ['RIGHT'] * max(0, nc - c) + ['LEFT'] * max(0, c - nc)
        moves.append('CROSS')
        r, c = nr, nc
    return moves


def cheats(s, codes):
    """From the main menu: Extras, Cheats, each code; back to the main menu."""
    s.presses('DOWN', 'DOWN', 'DOWN', after=40)
    s.press('CROSS', after=150)
    s.presses('DOWN', 'DOWN', 'DOWN', after=30)
    for code in codes:
        s.press('CROSS', after=120)
        for m in key_moves(code):
            s.press(m, hold=3, after=9)
        s.wait(120)                        # (a good code closes the keyboard)
    s.press('TRIANGLE', after=120)
    s.presses('UP', 'UP', 'UP', after=40)


# Played sessions (tools/playbot bmx.py, closed loop, measured by the game's numbers): each folder's script.txt is the replay.
SESSIONS = os.environ.get('BMX_SESSIONS', os.path.join(base.SCRATCH, 'bmxcorpus', 'sessions'))
ROUTES = {}
ENVIRONMENT = {}


def played(name, tail=None):
    """A session's script, then `tail` (a Script filled from the session's end)."""
    import json

    def build():
        s = Script()
        s.items = [open(os.path.join(SESSIONS, name, 'script.txt')).read().strip()]
        s.t = json.load(open(os.path.join(SESSIONS, name, 'meta.json')))['end'] + 30
        if tail:
            tail(s)
        return s
    return build


def pause_tour(s):
    """The pause menu: VIEW CHALLENGES, VIEW BIKE PARTS, VIEW RECORDS, OPTIONS, each and back, then END RUN."""
    s.press('START', after=90)
    for item in (1, 2, 3, 4):
        s.presses(*['DOWN'] * item, after=25)
        s.press('CROSS', after=150)
        s.presses('DOWN', 'DOWN', after=25)
        s.press('TRIANGLE', after=90)
    s.press('UP', after=30)                # the cursor wraps to END RUN
    s.press('CROSS', after=120)
    s.press('DOWN', after=30)
    s.press('CROSS', after=400)
    s.press('CROSS', after=300)
    s.presses('DOWN', 'DOWN', after=30)
    s.press('CROSS', after=300)
    s.press('CROSS', after=300)


def gameover_save(s):
    """(routes2) The run ended from the pause menu (its cursor on CONTINUE), the run's summary, GAME OVER: SAVE GAME to
    slot 1 (Yes), VIEW RECORDS, EXIT TO MAIN MENU (Yes)."""
    s.press('START', after=90)
    s.press('UP', after=40)                # (wraps to END RUN)
    s.press('CROSS', after=150)            # EXIT? NO
    s.press('DOWN', after=40)
    s.press('CROSS', after=400)            # YES: the run's summary
    s.press('CROSS', after=400)            # GAME OVER
    s.press('DOWN', after=40)
    s.press('CROSS', after=300)            # SAVE GAME: the slots
    s.press('CROSS', after=300)            # slot 1: are you sure? (No)
    s.press('UP', after=40)
    s.press('CROSS', after=700)            # Yes: saved, GAME OVER again
    s.press('DOWN', after=40)
    s.press('CROSS', after=200)            # VIEW RECORDS
    s.press('RIGHT', after=60)
    s.press('TRIANGLE', after=100)
    s.press('DOWN', after=40)
    s.press('CROSS', after=200)            # EXIT TO MAIN MENU: EXIT? NO
    s.press('DOWN', after=40)
    s.press('CROSS', after=900)            # YES: the main menu


def frontend():
    """The main menu's screens: Rider Editor, Extras, Options."""
    s = Script()
    s.presses('DOWN', after=40)
    s.press('CROSS', after=200)            # RIDER EDITOR: a custom rider, its screens and back
    s.press('CROSS', after=200)
    s.presses('RIGHT', 'RIGHT', 'DOWN', 'DOWN', after=40)
    s.press('TRIANGLE', after=120)
    s.press('TRIANGLE', after=120)
    s.press('TRIANGLE', after=120)
    s.presses('DOWN', after=40)
    s.press('CROSS', after=150)            # EXTRAS
    for item in (0, 1, 3, 4, 5, 6):        # HIGH SCORE, MOVIES, CHEATS, SPECIAL MODES, COMPETITIVE EDGE, ACCLAIM DEMOS
        s.presses(*['DOWN'] * item, after=25)
        s.press('CROSS', after=200)
        s.presses('DOWN', 'RIGHT', after=30)
        s.press('TRIANGLE', after=150)
        s.presses(*['UP'] * item, after=25)
    s.press('TRIANGLE', after=120)
    s.presses('DOWN', after=40)
    s.press('CROSS', after=150)            # OPTIONS, each row
    for item in range(5):
        s.press('CROSS', after=120)
        s.presses('RIGHT', 'LEFT', after=30)
        s.press('TRIANGLE', after=90)
        s.press('DOWN', after=30)
    s.press('TRIANGLE', after=120)
    return s


# The game's own exit (test only, as AI's cov-quit and dm2routes' quit-fe): engineMain's loop flag (0x252A94, the word
# requestQuit sets) poked, so the main loop ends and the game's and the engine's shutdown run, then Exit.
QUIT_FLAG = 0x252A94


def quit_env(at):
    """The loop flag set at tick `at` (PS2X_POKE_SCRIPT), the route's end 1,800 ticks on (the guest stops at its Exit)."""
    return {'PS2X_POKE_SCRIPT': '%x:%x=1' % (at, QUIT_FLAG), 'PS2X_DUMP_WHEN': 'end:tick>=%d' % (at + 1800)}


def quit_fe():
    """The main menu, then the exit: the shutdown from the front end."""
    s = Script()
    s.wait(600)
    s.press('DOWN', after=3000)
    return s


ROUTES['frontend'] = (frontend, 'the main menu: rider editor, extras, options')
ROUTES['frontend-hle'] = (frontend, 'as frontend with the game\'s own movie player (PS2X_MOVIES=hle): Extras, Movies')
ENVIRONMENT['frontend-hle'] = {'PS2X_MOVIES': 'hle'}
ROUTES['quit-fe'] = (quit_fe, "the game's exit from the main menu (main's loop flag poked: test only): the game and engine shutdown")
ENVIRONMENT['quit-fe'] = quit_env(1400 + 240 + 6 + 240 + 600)

def fe_card():
    """The front end's card and settings (routes2): OPTIONS, LOAD & SAVE: SAVE to slot 1 (Yes), LOAD from it (Yes);
    CONTROLS: vibration off, the configurations; AUDIO: each volume; EXTRAS: HIGH SCORE, MOVIES (the first played),
    SPECIAL MODES, COMPETITIVE EDGE."""
    s = Script()
    s.wait(200)                            # (the main menu takes presses a little after it shows)
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=40)   # (MULTIPLAYER, greyed with one pad, is a stop)
    s.press('CROSS', after=400)            # OPTIONS
    s.press('CROSS', after=150)            # LOAD & SAVE
    s.press('DOWN', after=40)
    s.press('CROSS', after=300)            # SAVE: the slots
    s.press('CROSS', after=300)            # slot 1: are you sure? (No)
    s.press('UP', after=40)
    s.press('CROSS', after=600)            # Yes: saved
    s.press('UP', after=40)
    s.press('CROSS', after=300)            # LOAD: the slots
    s.press('CROSS', after=300)            # slot 1: are you sure? (No)
    s.press('UP', after=40)
    s.press('CROSS', after=600)            # Yes: loaded
    s.press('TRIANGLE', after=90)
    s.press('DOWN', after=40)
    s.press('CROSS', after=200)            # CONTROLS
    s.presses('DOWN', 'RIGHT', 'RIGHT', 'LEFT', 'LEFT', 'UP', after=40)
    s.press('TRIANGLE', after=90)
    s.press('DOWN', after=40)
    s.press('CROSS', after=200)            # AUDIO
    s.presses('RIGHT', 'LEFT', 'DOWN', 'RIGHT', 'LEFT', 'DOWN', 'RIGHT', 'LEFT', after=40)
    s.press('TRIANGLE', after=90)
    s.press('TRIANGLE', after=300)         # the main menu
    s.press('UP', after=40)
    s.press('CROSS', after=400)            # EXTRAS
    s.press('CROSS', after=300)            # HIGH SCORE
    s.presses('RIGHT', 'DOWN', 'RIGHT', after=60)
    s.press('TRIANGLE', after=120)
    s.press('DOWN', after=40)
    s.press('CROSS', after=300)            # MOVIES
    s.press('CROSS', after=1200)           # the first movie
    s.press('START', after=300)
    s.press('TRIANGLE', after=120)
    s.presses('DOWN', 'DOWN', 'DOWN', after=40)
    s.press('CROSS', after=300)            # SPECIAL MODES
    s.presses('DOWN', 'CROSS', 'DOWN', after=60)
    s.press('TRIANGLE', after=120)
    s.press('DOWN', after=40)
    s.press('CROSS', after=300)            # COMPETITIVE EDGE
    s.presses('CROSS', 'DOWN', after=200)
    s.press('TRIANGLE', after=120)
    s.press('TRIANGLE', after=300)
    return s


ROUTES['fe-card'] = (fe_card, 'routes2: the front end card (save, load), controls, audio, extras')
ROUTES['fe-card-hle'] = (fe_card, 'routes2: as fe-card with the game\'s own movie player')
ENVIRONMENT['fe-card-hle'] = {'PS2X_MOVIES': 'hle'}


def fe_misc():
    """(routes2) The front end's other corners: CROSS on the greyed MULTIPLAYER and CREDITS, the RIDER EDITOR's screens
    (a rider made, its pages, back), CHEATS (a good code and a wrong one), HARDCORE TOUR's rider select flicked through
    and backed out of."""
    s = Script()
    s.wait(200)
    s.press('DOWN', after=40)
    s.press('CROSS', after=120)            # MULTIPLAYER (greyed)
    s.press('DOWN', after=40)
    s.press('CROSS', after=400)            # RIDER EDITOR
    for b in ('CROSS', 'RIGHT', 'CROSS', 'DOWN', 'RIGHT', 'CROSS', 'DOWN', 'LEFT', 'CROSS'):
        s.press(b, after=150)
    for _ in range(4):
        s.press('TRIANGLE', after=200)
    s.press('DOWN', after=40)
    s.press('CROSS', after=400)            # EXTRAS
    s.presses('DOWN', 'DOWN', after=40)
    s.press('CROSS', after=120)            # CREDITS (greyed)
    s.press('DOWN', after=40)
    s.press('CROSS', after=120)            # CHEATS: the keyboard
    for m in key_moves('ZZZ'):
        s.press(m, hold=3, after=9)
    s.wait(120)
    s.press('CROSS', after=120)
    for m in key_moves('THATDAMLEVEL'):
        s.press(m, hold=3, after=9)
    s.wait(200)
    s.press('TRIANGLE', after=120)
    s.press('TRIANGLE', after=400)         # the main menu
    s.presses('UP', 'UP', 'UP', 'UP', after=40)
    s.press('CROSS', after=400)            # HARDCORE TOUR: the riders
    s.presses('RIGHT', 'RIGHT', 'LEFT', 'DOWN', 'UP', after=120)
    s.press('TRIANGLE', after=400)
    return s


ROUTES['fe-misc'] = (fe_misc, 'routes2: the front end: greyed items, the rider editor, cheats, the rider select')


def fe_extras():
    """(routes3) The extras the cheats open (the cheat table g_cheatCodes, ELF 0x2863F0: CHAMPAGNE ROOM every movie,
    GHOSTCONTROL feature 0 (guided ghost rides) opened and on, 3RD SOG feature 3 (night vision) opened and on, Z AXIS
    movie 1): EXTRAS (its cursor kept on CHEATS) MOVIES: the first two movies picked (the media viewer's file name)
    and skipped with START; CREDITS; SPECIAL MODES: both features turned off and the first on again; COMPETITIVE EDGE
    and back. The menus keep their cursor: every move here is from the place the last left it."""
    s = Script()
    s.wait(200)
    cheats(s, ['CHAMPAGNE ROOM', 'GHOSTCONTROL', '3RD SOG', 'Z AXIS'])
    s.presses('DOWN', 'DOWN', 'DOWN', after=40)
    s.press('CROSS', after=300)            # EXTRAS, on CHEATS
    s.presses('UP', 'UP', after=40)
    s.press('CROSS', after=300)            # MOVIES: the list, its first line
    s.press('CROSS', after=900)            # the first movie
    s.press('START', after=400)
    s.press('DOWN', after=60)
    s.press('CROSS', after=900)            # the second
    s.press('START', after=400)
    s.press('TRIANGLE', after=200)
    s.press('DOWN', after=40)
    s.press('CROSS', after=900)            # CREDITS
    s.press('TRIANGLE', after=200)
    s.presses('DOWN', 'DOWN', after=40)
    s.press('CROSS', after=300)            # SPECIAL MODES
    s.press('CROSS', after=100)            # GUIDED GHOSTRIDES off
    s.presses('DOWN', 'DOWN', 'DOWN', after=40)
    s.press('CROSS', after=100)            # NIGHT VISION MODE off
    s.presses('UP', 'UP', 'UP', after=40)
    s.press('CROSS', after=100)            # GUIDED GHOSTRIDES on
    s.press('TRIANGLE', after=200)
    s.press('TRIANGLE', after=400)         # the main menu
    return s


ROUTES['fe-extras'] = (fe_extras, 'routes3: cheats for the movies and features, the media viewer, credits, special modes')

def menus():
    """The app's Settings page (PS2X_GAME_SETTINGS_MENUS=1, bmx_xxx_settings.cpp) from Options (its fourth line,
    SETTINGS) and from the pause menu in the Bronx (its last line), every section and row stepped Right and Left
    (dm2routes' settings_walk); back out each time."""
    s = Script()
    s.wait(200)                            # (the main menu takes presses a little after it shows)
    s.presses('DOWN', 'DOWN', 'DOWN', 'DOWN', after=40)   # (MULTIPLAYER, greyed with one pad, is a stop)
    s.press('CROSS', after=400)            # OPTIONS
    s.presses('DOWN', 'DOWN', 'DOWN', after=40)           # SETTINGS
    s.press('CROSS', after=200)
    base.settings_walk(s, MENUS_ROWS)
    s.press('TRIANGLE', after=200)         # Options, on SETTINGS
    s.press('TRIANGLE', after=400)         # the main menu, on OPTIONS
    s.presses('UP', 'UP', 'UP', 'UP', after=40)           # HARDCORE TOUR
    s.press('CROSS', after=240)            # HARDCORE TOUR
    s.press('CROSS', after=600)            # the rider
    s.press('CROSS', after=300)            # the Bronx
    s.wait(300)
    s.sync('0x%X==121' % LEVEL_FRAME)
    s.wait(60)
    s.press('CROSS', after=120)            # the level's box (THE BRONX, NYC): CONTINUE
    s.press('CROSS', after=120)            # CHALLENGES: CONTINUE
    s.press('START', after=90)             # the pause menu
    s.press('UP', after=40)                # (wraps to its last line, SETTINGS)
    s.press('CROSS', after=200)
    base.settings_walk(s, MENUS_ROWS)
    s.press('TRIANGLE', after=200)         # the pause menu again
    s.press('START', after=200)            # riding
    s.wait(300)
    return s


# The Settings page's rows in each section the runner has (Sound, Controls, Camera, Assists, Cheats: dm2routes' MENUS_ROWS).
MENUS_ROWS = (3, 46, 3, 2, 3)
ROUTES['menus'] = (menus, "the app's Settings page (PS2X_GAME_SETTINGS_MENUS=1) from Options and the pause menu: every section, Right and Left on every row")
ENVIRONMENT['menus'] = {'PS2X_GAME_SETTINGS_MENUS': '1', 'PS2X_TRACE_SETTINGS': '1'}  # (the trace: the page's keys, which the A/B reads back)

if os.path.isdir(SESSIONS):
    import json as _json
    for _name in sorted(os.listdir(SESSIONS)):
        if os.path.exists(os.path.join(SESSIONS, _name, 'script.txt')):
            ROUTES[_name] = (played(_name, pause_tour if _name.endswith('-end') else gameover_save if _name.endswith('-gos') else None), 'played: ' + _name)
            # a session's own environment (meta.json's env: a recording's start, a test-only poke), as dm2routes' played routes
            _meta = os.path.join(SESSIONS, _name, 'meta.json')
            if os.path.exists(_meta):
                ENVIRONMENT[_name] = dict(_json.load(open(_meta)).get('env', {}))


def script_of(name):
    return ROUTES[name][0]().text()


def extra_env(name):
    return ENVIRONMENT.get(name, {})


def main():
    a = sys.argv[1:]
    if a and a[0] == 'routes':
        for k, v in ROUTES.items():
            print('%-16s %s' % (k, v[1]))
    elif a and a[0] == 'script':
        print(script_of(a[1]))
    elif a and a[0] == 'env':              # (BMX's own: dm2routes' main would print DM2's)
        for k, v in extra_env(a[1]).items():
            print('%s=%s' % (k, '' if v is None else v))
    else:
        base.main()


if __name__ == '__main__':
    main()
