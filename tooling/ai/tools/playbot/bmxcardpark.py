#!/usr/bin/env python3
"""BMX XXX's Park Editor with the park on the PS2 memory card (routes1): bmxedit.py's session in by the main menu's
PARK EDITOR line (the collection's park editor extra, PS2X_BMX_PARK_EDITOR=1) with the game's own card jobs
(PS2X_SAVES=card), and the park loaded back both ways.

    bmxcardpark.py NAME [SEED [EDITS]]

The run: NEW PARK, the terrain raised and lowered, every tool toured (each with the d-pad's four ways, the shoulders,
the face buttons, and L3, R3, SELECT), pieces placed, a test run ridden, more pieces, SAVE to card slot 1; the pause's
LOAD PARK (the card's parks listed, the one saved picked: the park loaded into the running editor); CLEAR PARK; EXIT;
then the front end's PARK EDITOR, LOAD PARK (the same park, into a new editor), a few edits, EXIT; and PARK EDITOR,
RIDE PARK, the park ridden (bmxedit's ride_saved). GS=1: pictures at each dialog (bmxedit's shots). Numbers as
bmxedit's (stats.json: pieces placed and deleted, the test run's and the ride's distance and jumps, the park saves on
the card).
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bmxedit  # noqa: E402
from core import pad  # noqa: E402

# The editor's pause menu (bmxedit.pause_item's lines): 1 TEST RUN, 2 SAVE, 3 CLEAR PARK, 4 LOAD PARK, 5 EXIT.
PAUSE_LOAD = 4


class CardParkBot(bmxedit.Bot):
    def extra_buttons(self):
        """The buttons bmxedit's tour never presses, on the tool at hand: L3, R3, SELECT."""
        for b in ('L3', 'R3', 'SELECT'):
            yield from self.tap(b, hold=6, gap=40)
            yield from self.tap('TRIANGLE', hold=6, gap=40)
        self.stats.count('extra_buttons')

    def card_pick(self, what, presses=4):
        """A card dialog chain (the slot, the park list, a confirm): CROSS, each after its screen has had time."""
        for k in range(presses):
            yield from self.wait(400)
            self.shot('%s-%d' % (what, k))
            yield from self.tap('CROSS', hold=10, gap=60)
        back = yield from self.until(self.editing, 3000)
        yield from self.wait(300)
        self.shot('%s-done' % what)
        self.log(self.st, '%s: %s' % (what, 'back in the editor' if back else 'NOT back in the editor'))
        self.stats.count(what + ('_ok' if back else '_unsure'))

    def load_in_editor(self):
        yield from self.until(self.editing, 600)
        yield from self.pause_item(PAUSE_LOAD)
        self.shot('load-confirm')
        yield from self.tap('UP', gap=40)               # (a YES above NO confirm, if it asks)
        yield from self.card_pick('editor-load')

    def front_end_load(self):
        """The main menu's PARK EDITOR (third line), LOAD PARK (its second), the card's park."""
        yield from self.wait(240)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('DOWN', gap=40)
        yield from self.tap('CROSS', hold=8, gap=200)
        yield from self.tap('DOWN', gap=40)
        self.shot('fe-load-line')
        yield from self.tap('CROSS', hold=8, gap=300)
        ok = yield from self.until(lambda s: self.game_mode() == bmxedit.PARK_EDITOR, 1200)
        if not ok:
            yield from self.card_pick('fe-load', presses=3)
        else:
            yield from self.until(self.editing, 3000)
        self.log(self.st, 'front end LOAD PARK: %s' % ('in the editor' if self.editing(self.st) else 'NOT in the editor'))
        self.stats.count('fe_load_' + ('ok' if self.editing(self.st) else 'failed'))

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
        yield from self.tool_tour()
        yield from self.extra_buttons()
        yield from self.edits(edits)
        yield from self.test_run(1500)
        yield from self.edits(edits // 2)
        yield from self.save()
        self.shot('after-save')
        yield from self.load_in_editor()
        yield from self.edits(2)
        yield from self.clear_and_exit()
        yield from self.front_end_load()
        if self.editing(self.st):
            yield from self.edits(3)
            yield from self.exit_editor()
        yield from self.ride_saved(1500)


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    os.environ['BMXEDIT_MENU'] = '1'
    os.environ['PS2X_SAVES'] = 'card'
    bmxedit.Bot = CardParkBot
    bmxedit.play_session(a[0], int(a[1]) if len(a) > 1 else 1, int(a[2]) if len(a) > 2 else 12,
                         max_secs=int(os.environ.get('MAX_SECS', '3000')))
