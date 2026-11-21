#!/usr/bin/env python3
"""Aggressive Inline: a career level's in-level editor entered twice with its pieces still standing (routes2): the
Cannery's "Construct and Score 50,000 Points" (the ice machine's park). The second entry puts the pieces the first one
left back in the park (showEditCorePark -> restoreBreakables -> wrapEditDisplay for each piece not knocked down).

    [LEVEL=6] aieditor.py NAME [ROUNDS [WAIT]]   ROUNDS: the tool's rounds (the ice machine's of three pieces, 2);
                                       WAIT: ticks the skater stands off the pieces each time (6000). LEVEL (routes3):
                                       5 Cannery (the default), 6 Airfield's Vacuum Tool ("Place and Trick on 6 Items");
                                       ROUND=cov: Cannery's round as routes.py's cov-pisser presses it

Closed on the game's state: the giver found by the level's own objective record and data points (ai.objectives,
giver_of) and talked to (ai.talk, CIRCLE until the objective moves on); the editor is up while g_parkEditorCore is
set; its pause's Do challenge (entry 1) ends the editing (hideEditCorePark keeps the placed pieces). Then the giver
is talked to and the skater stands still (the pieces not ridden into) until the objective's state moves on, up to
WAIT ticks, and again, until a talk opens the editor. play.log: each step with its tick, the objective's state and
the editor's.

(routes2) As run: the objective stays Wait after the editing and two talks; it went to Active 9,000 ticks later
(what moved it is not known), and the next talk opened the editor again (tick 15,943): the second entry.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..', 'tools'))   # (tools/: zpaths.py)
import zpaths  # noqa: E402  (tools/paths.env; the playbot's folders, each game's and the shared one)
zpaths.use('playbot', 'aggressive-playbot', 'dm2-playbot', 'bmx-playbot', 'modding', 'checks', 'aggressive', 'dm2', 'bmx')
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ai  # noqa: E402
import core  # noqa: E402
from ai import tap, wait, until  # noqa: E402
from core import pad  # noqa: E402

CANNERY, AIRFIELD = 5, 6
TITLE = 'Construct and Score 50,000 Points'
# (routes3) the levels whose in-level editor this player knows: the objective's title (a prefix: the record's own
# text), the tool's round. LEVEL=6: Airfield, "Place and Trick on 6 Items", the 'B' room's Vacuum Tool, whose pieces
# the skater only touches (not knocked down: their breakable records keep state 1). The corpus's cov-vacuum, cov-pisser,
# cov-crane and cov-construct each made a breakable record at the editing's end (collectCellBreakable's block
# 0x1F6AD8, zx-fncheck --call-blocks), but none entered the editor again.
LEVEL = int(os.environ.get('LEVEL', str(CANNERY)))


def editing(bot):
    return bot.run.u32(ai.PE_EDITOR) != 0


def build_round(bot, i):
    """The ice machine's pieces (routes.py cr_tool's pisser, without its delete): the dispenser's piece changed
    (d-pad RIGHT), placed (TRIANGLE), moved up two cells, created (X), turned (O), back to the dispenser (TRIANGLE)."""
    for piece in range(3):
        for _ in range(piece + i):
            yield from tap(bot, 'RIGHT', hold=6, gap=24)
        yield from tap(bot, 'TRIANGLE', gap=50)
        yield from tap(bot, 'UP', gap=14)
        yield from tap(bot, 'UP', gap=14)
        yield from tap(bot, 'CROSS', gap=40)
        yield from tap(bot, 'CIRCLE', gap=24)
        yield from tap(bot, 'TRIANGLE', gap=50)


def pisser_round(bot, i):
    """(routes3, ROUND=cov) The ice machine's round as routes.py cr_tool's pisser has it (cov-pisser, whose editing's
    end made a breakable record): four pieces, each placed, created, turned, moved two cells and created again, one
    deleted, moved back and created, then back to the dispenser."""
    for piece in range(4):
        for _ in range(piece + i):
            yield from tap(bot, 'RIGHT', hold=6, gap=24)
        for button, gap in (('TRIANGLE', 50), ('UP', 14), ('UP', 14), ('CROSS', 40), ('CIRCLE', 24), ('RIGHT', 14),
                            ('RIGHT', 14), ('CROSS', 40), ('LEFT', 14), ('SQUARE', 40), ('DOWN', 14), ('CROSS', 40),
                            ('TRIANGLE', 50)):
            yield from tap(bot, button, hold=6, gap=gap)


def vacuum_round(bot, i):
    """The Vacuum Tool's round (routes.py cr_tool's vacuum): a snake over three rows of four cells, at each X (pick),
    two cells on, O (rotate), X (release), a cell on; two rows down after each row."""
    for row in range(3):
        side = 'RIGHT' if row % 2 == 0 else 'LEFT'
        for col in range(4):
            yield from tap(bot, 'CROSS', gap=30)
            yield from tap(bot, side, gap=12)
            yield from tap(bot, side, gap=12)
            yield from tap(bot, 'CIRCLE', gap=20)
            yield from tap(bot, 'CROSS', gap=30)
            yield from tap(bot, side, gap=12)
        yield from tap(bot, 'DOWN', gap=12)
        yield from tap(bot, 'DOWN', gap=12)


EDITORS = {CANNERY: ('Construct and Score', pisser_round if os.environ.get('ROUND') == 'cov' else build_round),
           AIRFIELD: ('Place and Trick', vacuum_round)}


# The edit selection's pieces (routes.py edit_pieces, round paths5): the list's head at 0x278B20, a node's record at
# node + 0x10 (+0x00 whole), the next node the node's first word.
EDIT_PIECES = 0x278B20


def pieces(bot):
    """(standing, all) of the edit selection's pieces."""
    run, n, whole = bot.run, 0, 0
    node = run.u32(run.u32(EDIT_PIECES)) if run.u32(EDIT_PIECES) else 0
    while node and n < 64:
        n += 1
        whole += 1 if run.u32(node + 0x10) else 0
        node = run.u32(node)
    return whole, n


def do_challenge(bot):
    """The editor's pause, Do challenge (entry 1)."""
    for _ in range(4):
        if bot.s.mode != ai.PAUSED:
            break
        yield from tap(bot, 'TRIANGLE', gap=40)
    yield from tap(bot, 'START', gap=60)
    yield from ai.vmenu(bot, 1)
    yield from tap(bot, 'CROSS', gap=60)
    yield from wait(bot, 300)
    yield from ai.close_boxes(bot)


def play(name, rounds=2, wait_ticks=6000):
    d = os.path.join(ai.SCRATCH, 'sessions', name)
    os.makedirs(d, exist_ok=True)
    log = open(os.path.join(d, 'play.log'), 'w')
    result = {'entries': 0, 'second': False}

    def say(bot, *what):
        o = objective(bot)
        log.write('%d %s | objective %s editor %d pieces %s\n' % (bot.s.tick, ' '.join(str(w) for w in what),
                                                                   o['state'] if o else '-', editing(bot), pieces(bot)))
        log.flush()

    def objective(bot):
        for o in ai.objectives(bot.run):
            if o['title'].startswith(EDITORS[LEVEL][0]):
                return o
        return None

    def enter(bot, plan, tries=3):
        o = objective(bot)
        pos = ai.giver_of(plan, o) if o else None
        say(bot, 'to the giver at', [round(v, 1) for v in pos] if pos else None)
        if pos is None:
            return False
        for attempt in range(tries):
            yield from ai.talk(bot, plan, objective(bot), pos)
            yield from until(bot, lambda s: editing(bot), 600)
            if editing(bot):
                result['entries'] += 1
                say(bot, 'in the editor, entry', result['entries'])
                return True
            say(bot, 'no editor after talking, attempt', attempt, 'skater at', [round(v, 1) for v in bot.s.pos],
                '%.1f from the giver' % ai.flat_dist(bot.s.pos, pos), 'object %#x' % (o or {}).get('object', 0))
        return False

    def behaviour(bot):
        yield from ai.boot(bot)
        yield from ai.career(bot, LEVEL)
        plan = ai.Plan(ai.nav.LEVELS[LEVEL])
        say(bot, 'objectives', [(o['title'], o['state']) for o in ai.objectives(bot.run)])
        yield from until(bot, lambda s: s.speed > 0.2, 900, pad('UP'))
        if not (yield from enter(bot, plan)):
            return
        yield from wait(bot, 200)
        for i in range(rounds):
            yield from EDITORS[LEVEL][1](bot, i)
            say(bot, 'round', i)
        yield from do_challenge(bot)
        say(bot, 'editing over')
        # Then the giver starts the challenge (Wait -> Active: its ninety seconds); the skater stands still (the
        # pieces not ridden into) until it is over, and the giver is talked to again for the editor.
        if os.environ.get('QUICK') == '1':
            # (routes3) the giver talked to again at once and again while the challenge runs (its pieces' records
            # are still in the breakable pool: the level script's cleanParkEditor frees them, cleanEditCore ->
            # freeBreakables, once the challenge is over), every 200 ticks for 5,400 ticks
            end = bot.s.tick + 5400
            while bot.s.tick < end:
                if (yield from enter(bot, plan, tries=1)):
                    result['second'] = True
                    yield from wait(bot, 300)
                    say(bot, 'second entry done (quick)')
                    return
                yield from wait(bot, 200)
            say(bot, 'quick: no second entry')
        for visit in range(4):
            state = (objective(bot) or {}).get('state')
            if (yield from enter(bot, plan, tries=1)):
                result['second'] = True
                yield from wait(bot, 300)
                say(bot, 'second entry done')
                return
            state2 = (objective(bot) or {}).get('state')
            say(bot, 'talked: %s -> %s' % (state, state2))
            # (routes2) the state moved on in one run after the skater had ridden off the giver: rolled a little
            # (UP, then DOWN to a stand) every 600 ticks while waiting.
            end = bot.s.tick + wait_ticks
            while bot.s.tick < end and (objective(bot) or {}).get('state') == state2 and not editing(bot):
                yield from until(bot, lambda s: False, 40, pad('UP'))
                yield from until(bot, lambda s: s.speed < 0.5, 200, pad('DOWN'))
                yield from until(bot, lambda s: (objective(bot) or {}).get('state') != state2 or editing(bot), 560)
            say(bot, 'waited')

    cfg = ai.config(name, {'ZX_DEV_UNLOCK_ALL': '1'}, secs=1500)
    with core.Run(cfg) as run:
        bot = ai.Player(run, behaviour, 1)
        core.play(run, ai.read, bot, 26000)
        print('notes', sorted(bot.notes.items(), key=lambda kv: -kv[1])[:20], flush=True)
    log.write('end: editor entries %d, second entry %s\n' % (result['entries'], result['second']))
    log.close()
    print(name, 'editor entries', result['entries'], 'second entry', result['second'])


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        raise SystemExit(__doc__)
    play(a[0], int(a[1]) if len(a) > 1 else 2, int(a[2]) if len(a) > 2 else 6000)
