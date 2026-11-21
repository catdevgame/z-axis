#!/usr/bin/env python3
"""BMX XXX's Park Editor with the park on the PS2 memory card (routes1): bmxedit.py's session in by the main menu's
PARK EDITOR line (the collection's park editor extra, ZX_BMX_PARK_EDITOR=1) with the game's own card jobs
(ZX_SAVES=card), and the park loaded back from the front end.

    bmxcardpark.py NAME [SEED [EDITS]]

The run: NEW PARK, the terrain raised and lowered, every tool toured (each with the d-pad's four ways, the shoulders,
the face buttons, and L3, R3, SELECT), pieces placed, a test run ridden, more pieces, SAVE to card slot 1; CLEAR PARK;
EXIT; then the front end's PARK EDITOR, LOAD PARK (slot 1, the park saved, the confirm's Yes: the cursor starts on No)
into a new editor, a few edits and a test run, EXIT; and PARK EDITOR's third line, the park ridden (bmxedit's
ride_saved). The editor's own pause has no LOAD PARK in this build (CONTINUE, TEST RUN, SAVE, CLEAR PARK, SOUND
OPTIONS, EXIT), so the park comes back only through the front end. GS=1: pictures at each dialog (bmxedit's shots).
Numbers as bmxedit's (stats.json: pieces placed and deleted, the test run's and the ride's distance and jumps, the
park saves on the card). CARDPARK_TOUR=0 leaves the tool tour out, CARDPARK_RUN sets the first test run's ticks
(1500), CARDPARK_PADPULL=1 pulls pad 1 out for 6 frames in the editor and halfway through the first test run (the
session's meta.json "control": dm2corpus.py pulls it at the same script time when it records the route; the route
pe-padpull, verzge1). The route pe-cardload (verzge1: seed 5, 10 edits, CARDPARK_TOUR=0, CARDPARK_RUN=900) is this run: it
reaches the load's readEditPiece, putHeldPiece, placePieceWithSelection, the tool set's camera and the test run's
pause.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmxedit  # noqa: E402
from core import pad  # noqa: E402

# The editor's pause menu (bmxedit.pause_item's lines): 1 TEST RUN, 2 SAVE, 3 CLEAR PARK, 4 SOUND OPTIONS, 5 EXIT.


class CardParkBot(bmxedit.Bot):
    def extra_buttons(self):
        """The buttons bmxedit's tour never presses, on the tool at hand: L3, R3, SELECT."""
        for b in ('L3', 'R3', 'SELECT'):
            yield from self.tap(b, hold=6, gap=40)
            yield from self.tap('TRIANGLE', hold=6, gap=40)
        self.stats.count('extra_buttons')

    def front_end_load(self):
        """The main menu's PARK EDITOR (third line), LOAD PARK (its second): card slot 1, the park, the confirm's Yes
        (above No, where the cursor starts)."""
        yield from self.wait(240)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('CROSS', hold=8, gap=200)
        yield from self.tap('DOWN', gap=40)
        self.shot('fe-load-line')
        yield from self.tap('CROSS', hold=8, gap=400)       # LOAD PARK
        self.shot('fe-load-slot')
        yield from self.tap('CROSS', hold=10, gap=400)      # MEMORY CARD slot 1
        self.shot('fe-load-park')
        yield from self.tap('CROSS', hold=10, gap=400)      # the park
        self.shot('fe-load-confirm')
        yield from self.tap('UP', gap=40)                   # Yes
        yield from self.tap('CROSS', hold=10, gap=200)
        ok = yield from self.until(lambda s: self.game_mode() == bmxedit.PARK_EDITOR, 3000)
        if ok:
            yield from self.until(self.editing, 3000)
        yield from self.wait(300)
        self.shot('fe-load-done')
        self.log(self.st, 'front end LOAD PARK: %s' % ('in the editor' if self.editing(self.st) else 'NOT in the editor'))
        self.stats.count('fe_load_' + ('ok' if self.editing(self.st) else 'failed'))

    def pad_pulled(self, what):
        """(CARDPARK_PADPULL=1) Pad 1 pulled out for 6 frames (core.Run.pull_pad: by the script clock, kept in the
        session's control schedule) and what the game does after it: in the editor a lost pad runs the tool set's row 0
        pressed (watchEditUiPad -> resetToolSetButton -> fireUiButton: the pause); in a test run the play screens'
        pad-out message. Then CONTINUE (the pause's first line) or START, back to where it was."""
        self.run.pull_pad(self.run.script_time + 20)
        yield from self.wait(120)
        self.shot('pad-pulled-' + what)
        self.log(self.st, 'pad pulled (%s): game mode %d, mode %d' % (what, self.game_mode(), self.st.mode))
        self.stats.count('pad_pulled_' + what)
        if what == 'editor':
            yield from self.tap('CROSS', hold=8, gap=120)   # CONTINUE
        else:
            yield from self.tap('START', hold=8, gap=120)
            self.shot('pad-pulled-%s-start' % what)
            if self.st.mode == 3:                           # (paused: CONTINUE)
                yield from self.tap('CROSS', hold=8, gap=120)
        self.shot('pad-pulled-%s-after' % what)

    def exit_editor(self):
        yield from self.until(self.editing, 600)
        yield from self.pause_item(5)                  # EXIT: YES above NO
        yield from self.tap('UP', gap=40)
        yield from self.tap('CROSS', hold=10, gap=60)
        out = yield from self.until(lambda s: s.mode == 1 and self.game_mode() == 0, 3000)
        self.log(self.st, 'the main menu again' if out else 'NOT at the main menu')

    def test_run(self, ticks):
        """bmxedit's test run, its way back as this build's test run pause has it: CONTINUE, OPTIONS, RETURN TO
        EDITOR, RESTART (RETURN TO EDITOR the third line), then EXIT? (NO above YES)."""
        yield from self.pause_item(1)                  # TEST RUN
        ok = yield from self.until(lambda s: s.rider, 3000)
        for _ in range(40):
            if self.st.pos != (0.0, 0.0, 0.0):
                break
            yield from self.tap('CROSS', gap=40)
        ok = ok and self.st.pos != (0.0, 0.0, 0.0)
        self.log(self.st, 'test run: rider %s' % ('in the park' if ok else 'NOT there'))
        if not ok:
            return
        meter = bmxedit.bmx_meter()
        for t in range(ticks):
            meter.tick(self.st)
            lx = 0.6 * ((t // 150) % 3 - 1)
            yield pad('UP', 'CROSS', lx=lx) if t % 240 < 8 else pad('UP', lx=lx)
            if t == ticks // 2 and os.environ.get('CARDPARK_PADPULL') == '1' and not self.stats.json().get('pad_pulled_run'):
                yield from self.pad_pulled('run')
        for k in ('distance', 'jumps'):
            self.stats.best('test_' + k, meter.s[k] if k in meter.s.json() else 0)
        self.log(self.st, 'test run over: distance %.0f jumps %d' % (meter.s.get('distance', 0), meter.s.get('jumps', 0)))
        yield from self.wait(60)
        yield from self.tap('START', gap=90)
        for _ in range(2):
            yield from self.tap('DOWN', hold=6, gap=25)
        self.shot('run-pause-return')
        yield from self.tap('CROSS', hold=10, gap=90)
        yield from self.tap('DOWN', gap=25)
        yield from self.tap('CROSS', hold=10, gap=60)
        back = yield from self.until(self.editing, 3000)
        self.log(self.st, 'back in the editor' if back else 'NOT back in the editor')
        self.stats.count('test_run_back' if back else 'test_run_not_back')

    def behaviour(self, edits):
        yield from self.menu_entry()
        ok = yield from self.until(lambda s: self.game_mode() == bmxedit.PARK_EDITOR, bmxedit.ENTRY + 3000)
        if not ok:
            self.log(self.st, 'NOT in the park editor')
            return
        yield from self.until(self.editing, 3000)
        yield from self.wait(300)
        self.log(self.st, 'in the park editor')
        yield from self.terrain()
        if os.environ.get('CARDPARK_TOUR', '1') == '1':
            yield from self.tool_tour()
            yield from self.extra_buttons()
        yield from self.edits(edits)
        if os.environ.get('CARDPARK_PADPULL') == '1':
            yield from self.pad_pulled('editor')
        yield from self.test_run(int(os.environ.get('CARDPARK_RUN', '1500')))
        yield from self.edits(edits // 2)
        yield from self.save()
        self.shot('after-save')
        yield from self.wait(300)
        yield from self.tap('TRIANGLE', gap=120)            # (the pause left open after the save)
        yield from self.clear_and_exit()
        yield from self.front_end_load()
        if self.editing(self.st):
            yield from self.edits(4)
            yield from self.test_run(600)
            yield from self.exit_editor()
        yield from self.ride_saved(1500)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    os.environ['BMXEDIT_MENU'] = '1'
    os.environ['ZX_SAVES'] = 'card'
    bmxedit.Bot = CardParkBot
    bmxedit.play_session(a[0], int(a[1]) if len(a) > 1 else 1, int(a[2]) if len(a) > 2 else 12,
                         max_secs=int(os.environ.get('MAX_SECS', '3000')))
