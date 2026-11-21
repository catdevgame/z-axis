#!/usr/bin/env python3
"""BMX XXX's front end: the rider select's option button and the rider editor's pages (routes3), closed on the menu
machine's own id (g_menu, game/bmx/ui/menu.h Menu::id; the ids game/bmx/states/front_end_menus.h FrontEndMenu).

    bmxrideredit.py NAME [TICKS]

The way: the title's START; the main menu's HARDCORE TOUR (its line by g_mainMenuChoices); at the rider select SQUARE
(the option button: FrameRiderSelect's KeySniffer OnOption, which opens the editor's home on that rider) and L1 held a
second there (the menu model turned), then back (TRIANGLE) to the rider select and the main menu; RIDER EDITOR (its line by g_mainMenuChoices), the first custom slot (CROSS), then the
editor home's buttons in turn: the home's button is an animation's value the player cannot read, so each CROSS is
judged by the menu it opens (BIO 42, STYLE 43, BODY SIZE 73, GENDER 71) and RIGHT takes the next button. On BIO's name
page the keyboard's END is looked for (START, then the keys' last row) until the home comes back (keepCharacterNames);
on GENDER, CROSS (its page: showGenderChoice) and the confirm's YES (line 0: changeCharacterGender) until the home
comes back from the confirm. Then TRIANGLE back to the main menu, and the run ends.

play.log in sessions/NAME: each menu change with its tick and what was pressed.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmx  # noqa: E402
import core  # noqa: E402
import goalsteer  # noqa: E402

# FrontEndMenu ids (states/front_end_menus.h)
PRESS_START, TITLE_LANGUAGE, MAIN_MENU, SAVE_WAIVER = 1, 2, 4, 5
RIDER_SELECT, LEVEL_SELECT = 7, 11
SELECT_CUSTOM, EDIT_HOME, RIDER_NAME, STYLE = 40, 41, 42, 43
GENDER, GENDER_CONFIRM, BODY_SIZE = 71, 72, 73
# MainMenuChoice (front_end_menus.h)
CHOICE_TOUR, CHOICE_RIDER_EDITOR = 0, 3
MAIN_MENU_LINES = 8


def play(name, ticks=30000):
    sym = goalsteer.Symbols('bmx')
    # (a pad script that only keeps the clock: core.play asks the policy once per script tick)
    gs = os.environ.get('GS') == '1'     # (GS=1: a picture of each name-page try)
    cfg = bmx.run_config(name, '99999999v:SELECT:1v', env={'ZX_EE_SCALE': '4096'}, max_secs=1500, gs=gs)
    d = bmx.session_dir(name)
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, 'play.log'), 'w')
    v = {'menu': None, 'since': 0, 'program': [], 'stage': 'tour', 'pages': [], 'name_tries': 0, 'home_tries': 0,
         'done': set()}

    def say(st, *what):
        log.write('%d menu %s | %s\n' % (st.script, v['menu'], ' '.join(str(w) for w in what)))
        log.flush()

    def queue(*items):
        v['program'].extend(bmx.presses(*items))

    def main_line(run, choice):
        choices = list(run.mem(sym['g_mainMenuChoices'], MAIN_MENU_LINES))
        return choices.index(choice) if choice in choices else None

    def on_menu(st, menu, run):
        stage = v['stage']
        if menu in (PRESS_START,):
            queue(30, 'START', 60)
        elif menu in (TITLE_LANGUAGE, SAVE_WAIVER):
            queue(30, 'CROSS', 60)
        elif menu == MAIN_MENU:
            want = CHOICE_TOUR if stage == 'tour' else CHOICE_RIDER_EDITOR if stage == 'edit' else None
            if want is None:
                v['stage'] = 'end'
                return
            line = main_line(run, want)
            say(st, 'main menu choices', list(run.mem(sym['g_mainMenuChoices'], MAIN_MENU_LINES)), 'line', line)
            if line is None:
                v['stage'] = 'end'
                return
            # (the cursor comes back where it was left: UP to the top first)
            queue(60, *['UP'] * MAIN_MENU_LINES, *['DOWN'] * line, 'CROSS', 60)
        elif menu == RIDER_SELECT and stage == 'tour':
            # SQUARE (the option button) opens the editor's home on the rider selected; L1 held there turns the
            # menu model; then back
            say(st, 'rider select: SQUARE, L1 held')
            v['program'].extend(bmx.presses(90, 'SQUARE', 90))
            v['program'].extend([core.pad('L1')] * 60 + [core.NOTHING] * 30)
            v['stage'] = 'select-home'
        elif menu == RIDER_SELECT:
            queue(60, 'TRIANGLE', 90)
        elif menu == LEVEL_SELECT:
            queue(30, 'TRIANGLE', 60)
        elif menu == SELECT_CUSTOM:
            if stage == 'edit':
                queue(90, 'CROSS', 60)
            else:
                queue(60, 'TRIANGLE', 60)
        elif menu == EDIT_HOME and stage == 'select-home':
            v['stage'] = 'edit'
            queue(60, 'TRIANGLE', 90)
        elif menu == EDIT_HOME:
            if v['done'] >= {RIDER_NAME, GENDER} or v['home_tries'] > 12:
                say(st, 'home: done', sorted(v['done']))
                v['stage'] = 'back'
                queue(60, 'TRIANGLE', 90)
                return
            v['home_tries'] += 1
            v['last_home'] = st.script
            queue(60, 'CROSS', 60)
        elif menu in (STYLE, BODY_SIZE):
            say(st, 'not the page wanted: back, RIGHT')
            queue(60, 'TRIANGLE', 90, 'RIGHT', 60)
            v['program'].append(None)          # (marker: the home's CROSS again once back)
        elif menu == RIDER_NAME:
            if RIDER_NAME in v['done']:
                v['name_back'] = True
                queue(60, 'TRIANGLE', 90, 'RIGHT', 60)
                return
            v['name_tries'] += 1
            # the name keyboard (seen with GS=1: 12 keys a row, six rows, A the first and the cursor on it; the last
            # row ends SPC DEL END): the cursor wraps at the edges (UP 7, LEFT 13, DOWN 5, RIGHT 11 from A ended on
            # '7'), so END, the bottom right key, is one UP and one LEFT from A
            k = v['name_tries']
            if gs:
                say(st, 'shot', run.shot('name-%d' % k))
            if k == 1:
                queue(90, 'UP', 30, 'LEFT', 30, 'CROSS', 90)
            else:
                say(st, 'name: no END found')
                v['done'].add(RIDER_NAME)
                v['name_back'] = True              # (TRIANGLE: back, not END)
                queue(60, 'TRIANGLE', 90)
        elif menu == GENDER:
            if GENDER in v['done']:
                queue(60, 'TRIANGLE', 90, 'RIGHT', 60)
                return
            queue(90, 'CROSS', 60)
        elif menu == GENDER_CONFIRM:
            v['confirm_tries'] = v.get('confirm_tries', 0) + 1
            queue(60, *(['UP'] * 2 if v['confirm_tries'] == 1 else ['DOWN']), 'CROSS', 60)
        elif stage == 'back' or stage == 'end':
            queue(60, 'TRIANGLE', 60)
        else:
            say(st, 'menu %s: CROSS' % menu)
            queue(60, 'CROSS', 60)

    def policy(st):
        run = run_box['run']
        menu = struct_i32(run.mem(sym['g_menu'], 4))
        if menu != v['menu']:
            old, v['menu'], v['since'] = v['menu'], menu, st.script
            v['pages'].append(menu)
            say(st, 'from', old)
            if old == RIDER_NAME and menu == EDIT_HOME and v.get('name_back') is None:
                v['done'].add(RIDER_NAME)
                say(st, 'name page left for the home (END, keepCharacterNames, after try %d)' % v['name_tries'])
            v['name_back'] = None
            if old == GENDER_CONFIRM and menu == EDIT_HOME:
                v['done'].add(GENDER)
                say(st, 'gender confirmed (changeCharacterGender)')
            if v['stage'] == 'back' and menu == MAIN_MENU:
                v['stage'] = 'end'
                v['end_at'] = st.script
        if v['stage'] == 'end':
            return None if st.script - v.get('end_at', st.script) > 300 else core.NOTHING
        while v['program']:
            p = v['program'].pop(0)
            if p is None:
                if v['menu'] == EDIT_HOME:
                    v['program'][:0] = bmx.presses(60, 'CROSS', 60)
                continue
            if not v['program']:
                v['since'] = st.script      # (a press's menu change can take a moment)
            return p
        if st.script > ticks:
            say(st, 'out of ticks')
            return None
        if menu >= 0 and st.script - v['since'] >= 60:
            v['since'] = st.script + 240       # (asked again after 5 s on the same menu)
            on_menu(st, menu, run)
            if v['program']:
                return v['program'].pop(0) or core.NOTHING
        return core.NOTHING

    def struct_i32(b):
        import struct
        return struct.unpack('<i', b)[0]

    run_box = {}
    with core.Run(cfg) as run:
        run_box['run'] = run
        core.play(run, bmx.read, policy, max_ticks=ticks + 2000)
    log.write('end at tick %d: pages %s, done %s\n' % (run.tick, v['pages'], sorted(v['done'])))
    log.close()
    print(name, 'pages', v['pages'], 'done', sorted(v['done']))


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]) if len(a) > 1 else 30000)
