#!/usr/bin/env python3
"""The shared recording corpus (docs/READABLE.md, "How a
function is proven"): every registered function (Verified to Planned) recorded over a
fixed set of routes, once per recorder version, in a scratch store that
rounds check against instead of recording again.

    readable_corpus.py record [ROUTE ...]   record the routes (all by default) into <store>/v<N>/<route>/
    readable_corpus.py index                (re)write <store>/v<N>/index.json and index.txt
    readable_corpus.py status               the index's summary: routes, sizes, uncovered functions
    readable_corpus.py folders FUNCTION     the folders that cover a function (for zx-fncheck)
    readable_corpus.py routes               the routes and what they do
    readable_corpus.py situations           the game situation each route plays (SITUATIONS: pruning)

Options:
  --store DIR     default $ZAXIS_SCRATCH/reccorpus (the version folder is added; outside
                  /tmp, which macOS clears at a restart)
  --build DIR     the z-axis build with the runner and zx-fncheck (required: your
                  worktree's own, never the main checkout's): its registry decides
                  what is recorded
  --jobs N        routes recorded side by side (default 2; each runner takes 2 GB and
                  more as its recording grows, and one core)
  --max-secs S    time limit of one pass (default 600: routes are short segments); the
                  runner is killed (by PID) at the limit, and the recordings it made so
                  far are kept
  --max-rss-mb M  a runner whose resident memory passes M (default 8000) is stopped
                  the same way; each pass logs its peak (record.log, runs.json)
  --routes-py F   the tools/routes.py the route runs use (default the workspace's;
                  READABLE_ROUTES_PY); runs get ROUTES_SLOW=8 (its wall-clock limits
                  eight times longer: a recording runner is slow)

N is the recorder's corpus version (fncheck.h kCorpusVersion: the file
version whose records it writes; version 6 files, which name their game in
the header, are version 5's corpus): recordings of another version are
another corpus. Since round 14 the recorder learns
what callees write from page protection (about 0.1 ms a callee call, against
the full RAM diff's 6 to 8 ms), so each route is one pass (`--passes all`,
the default) with a cap per function in the function list (ZX_FNREC=@file):
  light   every registered function but the heavy ones: 200 calls, at most
          8 a vsync;
  heavy   functions whose calls make many calls (an average of 8 or more in
          the store's earlier recordings, or 10 or more call sites when never
          recorded): 50 calls, 1 a vsync (spread over the run).
`--passes light,heavy` records them in two runs as before (1000 / 8 and
100 / 1).

Coverage routes (round covrec, the `cov-` routes): `--new-blocks SEED` records
with ZX_FNREC_NEW_BLOCKS=1 and ZX_FNREC_COVERED=SEED (fncheck_corpus.py
coverage --covered-out): a call is kept only if it runs a block of its
original that neither the seed (the corpus so far) nor a kept call ran, so a
route can try every call (up to NB_PER_VSYNC a vsync, 1 for heavy functions)
for as long as it plays and keeps only the new paths. `--mash-coverage FILE`
(fncheck_corpus.py coverage --json) weights mash's actions and spots by the
uncovered blocks each skater state reaches (routes.py MASH_COVERAGE). All passes: ZX_FNREC_ATOMIC=1, ZX_READABLE=off, unpaced (ZX_UNPACED=1),
the null GS and VU1 skipped (but the route gs, which draws), a scratch HOME,
card and support folder per run.

The index lists per route the functions recorded (calls kept, the callee
calls they made, MB) and, over the whole store, the registered functions no
route reached ("uncovered": a route to them is the next step, or they are
never reached, like Wall Tap's callbacks).
"""
import argparse
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')

HERE = os.path.dirname(os.path.abspath(__file__))
ZAXIS_TREE = ztree.TREE
WORKSPACE = zaxis_paths.get('AI_WORKSPACE', required=False) or ''  # tools/, recomp/ (its data links)
sys.path.insert(0, HERE)
import fnrec_index  # noqa: E402

# Segments that start in a level: nothing recorded in the front end (boot,
# the main menu: about vsync 800; the lc and career routes record it), so a
# 600 s pass reaches play, and the heavy pass's 100 calls are not all spent
# before it.
FRONT_END_SKIPPED = {'ZX_FNREC_FROM': '800'}
# Routes: (kind, what, stop tick or None, extra environment, description).
#   pad     a pad script (tools/<what>.py script), vsync clock, to the stop tick
#   input   a played session, ab_inputs/<what>.txt (tools/playbot/ai.py), to the stop tick
#   route   tools/routes.py commands (closed loop on the game's state)
ROUTES = {
    'lc': ('pad', 'logiccheck', 11400, {}, 'the logiccheck input: front end, the Tutorial and its goals'),
    'career': ('pad', 'careercheck', 5000, {}, 'the careercheck input: the Movie Lot career, the pause menu, a save'),
    'moves': ('route', 'boot; tutorial; moves', None, {},
              'the Tutorial\'s lessons: stall, poles, skitch, wall ride and jump, vault, grind, manual, cess slide'),
    'grinds': ('route', 'boot; tutorial; grindtricks', None, {},
               'grind tricks combined on the rail (grindx: the grind enter\'s paths)'),
    # (One input: port 1 mirrors it, as before pad port 1 went empty without a second gamepad.)
    'splitscreen': ('route', 'boot; splitscreen; skate 2', None, {'ZX_PAD_PORT1': 'mirror'},
                    'MULTIPLAYER\'s default game: split screen, skated'),
    'movielot': ('route', 'boot; career 1; skate 2; traffic 4; challenge complete', None, {},
                 'the Movie Lot career: skated, taxi hits, a challenge played to its end'),
    'museum': ('route', 'boot; career 7; doublegrind; skate 1', None, {}, 'the Museum career: a double grind, skated'),
    'spines': ('route', 'boot; freeskate 0; spines 4', None, {}, 'Movie Lot free skate: Air Spine both ways'),
    'levels-2': ('route', 'boot; freeskate 2; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 2, one round of skating (a segment of the old levels route)'),
    'levels-3': ('route', 'boot; freeskate 3; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 3, one round of skating (a segment of the old levels route)'),
    'levels-4': ('route', 'boot; freeskate 4; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 4, one round of skating (a segment of the old levels route)'),
    'levels-5': ('route', 'boot; freeskate 5; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 5, one round of skating (a segment of the old levels route)'),
    'levels-6': ('route', 'boot; freeskate 6; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 6, one round of skating (a segment of the old levels route)'),
    'levels-8': ('route', 'boot; freeskate 8; skate 1', None, FRONT_END_SKIPPED, 'free skate in level 8, one round of skating (a segment of the old levels route)'),
    'gs': ('route', 'boot; tutorial; skate 1', None, dict(FRONT_END_SKIPPED, ZX_GS_NULL=None, ZX_VU1_SKIP=None),
           'the Tutorial with the GS on (the CPU renderer): the renderers the null GS never calls'),
    # Round 12 (engine): routes to the functions the ten above never reached.
    'tricks-a': ('route', 'boot; tutorial; stalltrick UP,UP; stalltrick DOWN; stalltrick LEFT; stalltrick RIGHT; '
                 'skitch; skitchgo; hpole; vpolea', None, FRONT_END_SKIPPED,
                 'the Tutorial: the stall table\'s hand plants, both skitches, the poles'),
    'tricks-b': ('route', 'boot; tutorial; grindtricks - -/LEFT/LEFT/RIGHT/DOWN/UP -; grindtricks LEFT -/LEFT/LEFT/RIGHT/DOWN/UP -; grindtricks DOWN,DOWN -/UP/UP/DOWN/LEFT,LEFT/RIGHT,RIGHT -; grindtricks - RIGHT/RIGHT/LEFT/DOWN,RIGHT/RIGHT,DOWN/LEFT,DOWN -; grindtricks RIGHT LEFT/RIGHT/DOWN,LEFT -; grindtricks - RIGHT/UP,UP,UP/LEFT/UP,UP,UP -; grindtricks - RIGHT/~UP,UP,UP,UP/RIGHT/UP,UP,UP -; grindtricks - RIGHT/!3,RIGHT/!7,LEFT -; grindtricks - -/!5,UP/RIGHT,!4,DOWN -; grindtricks - RIGHT/RIGHT/LEFT/~UP,UP,UP,UP -', None, FRONT_END_SKIPPED,
                 'the Tutorial: grind tricks combined (readable-game11\'s press sequences, the first half)'),
    'tricks-c': ('route', 'boot; tutorial; grindtricks - RIGHT/~UP,!1,UP,UP,UP/!1,UP,UP,UP -; grindtricks - ~UP,LEFT,UP/LEFT,LEFT/LEFT,RIGHT/LEFT,DOWN/LEFT,UP+LEFT -; grindtricks - ~UP,RIGHT,UP+RIGHT/RIGHT,DOWN+RIGHT/LEFT,DOWN+LEFT/RIGHT,UP/RIGHT,DOWN -; grindtricks - ~UP,RIGHT,RIGHT/RIGHT,LEFT/RIGHT/RIGHT+DOWN/DOWN/DOWN+LEFT/LEFT -; grindtricks - ~UP,DOWN,DOWN,DOWN/UP+RIGHT/UP+LEFT -; grindtricks - RIGHT/RIGHT/LEFT/LEFT -; grindtricks - RIGHT/RIGHT/RIGHT/LEFT/RIGHT -; grindtricks LEFT LEFT/RIGHT/LEFT/RIGHT -; grindtricks - ~UP,RIGHT,RIGHT,!J,UP/RIGHT,!J,UP,UP/RIGHT,RIGHT,!J,DOWN,DOWN/!J,DOWN,DOWN,DOWN -; grindtricks - ~UP,RIGHT,!J,DOWN,DOWN/RIGHT,!J,DOWN,DOWN -', None, FRONT_END_SKIPPED,
                 'the Tutorial: grind tricks combined (the second half, with the !J unlocks last)'),
    'doublegrind': ('route', 'boot; career 7; ' + 'doublegrind; doublegrind; doublegrind; doublegrind UP 90 letgo; doublegrind UP 60 L1; doublegrind UP 30 raw:up_TRIANGLE,wait_1,press_TRIANGLE_2_1,wait_2,press_LEFT_2_2,press_TRIANGLE_2_1,wait_2,press_UP_2_2,press_UP_2_2,press_UP_2_2,press_TRIANGLE_2_1; doublegrind DOWN; doublegrindat 11.4 -39.2 0.25 -0.97; doublegrind', None, FRONT_END_SKIPPED,
                    'the Museum career: Double Grinds (let go, a spin button, a trick on it), a Hand Grind, the ring'),
    'careers-a': ('route', 'boot; career 1; challenge fail; score 999999000 2000; xp 0 99; skate 1', None, FRONT_END_SKIPPED,
                  'the Movie Lot career: Kelly\'s challenge failed, a score rollover and a level up'),
    'careers-b': ('route', 'boot; career 2; skate 1; menu; career 3; skate 1', None, FRONT_END_SKIPPED,
                  'the Civic Center and Industrial careers skated'),
    'careers-c': ('route', 'boot; career 4; skate 1; menu; career 5; skate 1; menu; career 6; skate 1', None, FRONT_END_SKIPPED,
                  'the Boardwalk, Cannery and Airfield careers skated'),
    'grindx': ('route', 'boot; tutorial; grindx back; grindx front 0.3 L1', None, FRONT_END_SKIPPED,
               'the Grinding lesson\'s grind with the enter\'s paths forced (the grinds route\'s second half)'),
}
# tools/routes.py `mash` in each level (docs/DEVTOOLS.md
# "mash"): one level load, then seeded play from the skater's state, a bail,
# a stop or a stay in a small area teleporting the skater to another of the
# level's spots. The careers (NPCs, goals, timers), the Zoo and Estate in free
# skate. Two runs of three minutes, seeds 100 + level and 200 + level; the
# juice kept up, the approach's velocity on a teleport, every trick unlocked
# (tweaks, logged in the route's mash-SEED.jsonl).
MASH_TWEAKS = 'tweaks=juice,push,tricks'
APP_RESOURCES = os.path.join(WORKSPACE, 'dist', 'Aggressive Inline.app', 'Contents', 'Resources')
ZOO_ON = {'ZX_ZOO': '1', 'ZX_XBOX_CONVERTED': os.path.join(APP_RESOURCES, 'xbox-converted')}
# Estate's files: the app's when it was built with a GameCube image, else READABLE_ESTATE_DIR.
ESTATE_ON = dict(ZOO_ON, ZX_ESTATE='1',
                 ZX_ESTATE_DIR=os.environ.get('READABLE_ESTATE_DIR') or os.path.join(APP_RESOURCES, 'estate'))
for _k, _name in enumerate(('Tutorial', 'Movie Lot', 'Civic Center', 'Industrial', 'Boardwalk', 'Cannery',
                            'Airfield', 'Museum')):
    ROUTES['mash-%d' % _k] = ('route', 'boot; career %d; mash 10800 %d %s; mash 10800 %d %s' % (
        _k, 100 + _k, MASH_TWEAKS, 200 + _k, MASH_TWEAKS), None, FRONT_END_SKIPPED,
        'mash: the %s career, six minutes of seeded play' % _name)
ROUTES['mash-zoo'] = ('route', 'boot; freeskate 7; mash 10800 108 %s; mash 10800 208 %s' % (MASH_TWEAKS, MASH_TWEAKS), None,
                      dict(FRONT_END_SKIPPED, **ZOO_ON), 'mash: the Zoo in free skate (ZX_ZOO, the app\'s Xbox files), six minutes')
ROUTES['mash-estate'] = ('route', 'boot; freeskate 8; mash 10800 109 %s; mash 10800 209 %s' % (MASH_TWEAKS, MASH_TWEAKS),
                         None, dict(FRONT_END_SKIPPED, **ESTATE_ON),
                         'mash: Estate in free skate (ZX_ESTATE, the app\'s files; the Zoo on before it), six minutes')
# Round covrec: routes for the paths no route above reaches, recorded with
# --new-blocks (only calls that run a block of the original no recording ran
# are kept). Longer mash runs per level, weighted by the uncovered blocks
# (--mash-coverage), and the test-only set-ups (pokes) of routes.py for the
# situations the worst covered Verified functions need.
COV_MASH = 'tweaks=juice,push,tricks visit=900'
for _k in range(8):
    ROUTES['cov-mash-%d' % _k] = ('route', 'boot; career %d; mash 21600 %d %s' % (_k, 300 + _k, COV_MASH), None,
                                  FRONT_END_SKIPPED,
                                  'covrec: mash in career %d, six minutes (or 540 s), weighted by the uncovered blocks' % _k)
ROUTES['cov-mash-zoo'] = ('route', 'boot; freeskate 7; mash 21600 308 %s' % COV_MASH, None,
                          dict(FRONT_END_SKIPPED, **ZOO_ON), 'covrec: mash in the Zoo, six minutes (or 540 s)')
ROUTES['cov-poles'] = ('route', 'boot; tutorial; ' + '; '.join(
    ['poleaim540 %s' % b for b in ('LEFT', 'RIGHT', 'UP', 'DOWN')] +
    ['poleaima540 %s' % b for b in ('LEFT', 'RIGHT', 'UP', 'DOWN')] +
    ['poleaim540 %s 0 1 6 %d' % (b, t) for b, t in (('LEFT', 90), ('RIGHT', -90), ('UP', 180))] +
    ['poleaim %s' % b for b in ('LEFT', 'RIGHT')] + ['hpole', 'vpole', 'vpolea']), None, FRONT_END_SKIPPED,
    'covrec: the Vertical Poles lesson with the unaimed swing poked (the aim runs), both sides, exits')
ROUTES['cov-stalls'] = ('route', 'boot; tutorial; ' + '; '.join(
    ['stalljump %s' % b for b in ('UP', 'DOWN', 'LEFT', 'RIGHT', '-', 'UP', 'DOWN', 'LEFT', 'RIGHT', '-')] +
    ['stalldrop', 'stalldrop', 'stalltrick UP,UP', 'stalltrick DOWN,DOWN', 'stalltrick LEFT,RIGHT', 'stalltrick UP,DOWN',
     'stalltrick DOWN,UP', 'stalltrick RIGHT,LEFT', 'stall', 'skitch', 'skitchgo', 'skitch', 'skitchgo']), None,
    FRONT_END_SKIPPED, 'covrec: Hand Plants jumped off with the stick each way, dropped, the stall trick table; skitches')
ROUTES['cov-grinds'] = ('route', 'boot; tutorial; ' + '; '.join(
    ['grindjump %s' % b for b in ('LEFT', 'RIGHT', '-', 'UP', 'DOWN', 'LEFT', 'RIGHT', '-')] +
    ['grindwall 20 120', 'grindwall 40 120', 'grindx back 0.5', 'grindx front -0.5 R1', 'grindx fwd 0 - 6',
     'grindx fwd 0 - 0 150 word']), None, FRONT_END_SKIPPED,
    'covrec: the Grinding lesson\'s grind jumped off each way, wall hits while grinding, the enter\'s paths')
ROUTES['cov-hazards'] = ('route', 'boot; ' + '; menu; '.join('career %d; hazardrun 6 120' % k for k in range(1, 8)), None,
                         FRONT_END_SKIPPED, 'covrec: the skater dropped on the levels\' water, fire and out-of-bounds faces')
# A second mash round (other seeds, the Estate too), recorded against the
# corpus with the first round's recordings in the seed.
for _k in range(8):
    ROUTES['cov-mash2-%d' % _k] = ('route', 'boot; career %d; mash 21600 %d %s' % (_k, 500 + _k, COV_MASH), None,
                                   FRONT_END_SKIPPED, 'covrec: mash in career %d, a second seed' % _k)
ROUTES['cov-mash2-zoo'] = ('route', 'boot; freeskate 7; mash 21600 508 %s' % COV_MASH, None,
                           dict(FRONT_END_SKIPPED, **ZOO_ON), 'covrec: mash in the Zoo, a second seed')
ROUTES['cov-mash2-estate'] = ('route', 'boot; freeskate 8; mash 21600 509 %s' % COV_MASH, None,
                              dict(FRONT_END_SKIPPED, **ESTATE_ON), 'covrec: mash in Estate')
_DG = lambda act: 'doublegrind UP 40 raw:' + act
ROUTES['cov-doublegrind'] = ('route', 'boot; career 7; doublegrind; doublegrind; ' + '; '.join([
    _DG('down_LEFT,wait_2,press_CROSS_3_1,up_LEFT'), _DG('down_RIGHT,wait_2,press_CROSS_3_1,up_RIGHT'),
    _DG('press_CROSS_3_1'), _DG('down_UP,wait_40,up_UP,down_DOWN,wait_40,up_DOWN,press_CROSS_3_1'),
    _DG('wait_1,press_CROSS_3_1'), 'doublegrind DOWN 40 raw:down_LEFT,wait_2,press_CROSS_3_1,up_LEFT',
    'doublegrind DOWN 40 raw:down_RIGHT,wait_2,press_CROSS_3_1,up_RIGHT', 'doublegrind DOWN 40 raw:press_CROSS_3_1',
    'doublegrindat 11.4 -39.2 0.25 -0.97 40 raw:down_LEFT,wait_2,press_CROSS_3_1,up_LEFT']), None, FRONT_END_SKIPPED,
    'covrec: Double and Hand Grinds jumped off with the stick each way, the balance pushed')
# (Measured unrecorded: a Double Grind lasts under 90 vsyncs and every
# other try is a plain Grind, so short holds, each way three times.)
ROUTES['cov-doublegrind2'] = ('route', 'boot; career 7; doublegrind; doublegrind; doublegrind; ' + '; '.join(
    '%s %s 20 raw:%s' % (('doublegrind', t, a)) for t in ('UP', 'DOWN') for a in (
        'down_LEFT,wait_2,press_CROSS_3_1,up_LEFT', 'down_RIGHT,wait_2,press_CROSS_3_1,up_RIGHT', 'press_CROSS_3_1',
        'down_UP,wait_20,up_UP,down_DOWN,wait_20,up_DOWN,press_CROSS_3_1') for _ in range(3)), None, FRONT_END_SKIPPED,
    'covrec: Double and Hand Grinds jumped off with the stick each way (short holds, three tries each)')
ROUTES['cov-grindpoke'] = ('route', 'boot; tutorial; grindpoke 3; grindpoke 0; grindpoke -1; grindpoke 0.01; grindpoke 6 30; '
                           'grindpoke -1 30; grindpoke 0 40', None, FRONT_END_SKIPPED,
                           'covrec: the Grinding lesson\'s grind with its velocity poked: past the top speed, stopped, back')
# The same set-ups again with the recorder's episode rule (a function's rest
# ends when it was not called the vsync before: a state's callback starts
# fresh in each move), so one-frame events in a short state are kept.
ROUTES['cov-holds-a'] = ('route', 'boot; tutorial; ' + '; '.join(
    ['stalljump %s %d' % (b, t) for t in (10, 40) for b in ('UP', 'DOWN', 'LEFT', 'RIGHT', '-')] +
    ['grindjump %s %d' % (b, t) for t in (10, 40) for b in ('LEFT', 'RIGHT', '-', 'UP')] +
    ['grindpoke 3', 'grindpoke 0', 'grindpoke -1', 'grindpoke 6 30'] +
    ['poleaim540 %s' % b for b in ('LEFT', 'RIGHT', 'UP', 'DOWN')] + ['hpole', 'hpole']), None, FRONT_END_SKIPPED,
    'covrec: hand plants and grinds jumped off each way, early and late, grind speeds poked, pole aims')
ROUTES['cov-holds-b'] = ('route', ROUTES['cov-doublegrind2'][1], None, FRONT_END_SKIPPED,
                         'covrec: cov-doublegrind2 again with the recorder\'s episode rule')
ROUTES['cov-pickups'] = ('route', 'boot; tutorial; lesson 22; skate 2; lesson 21; skate 1; lesson 20; skate 1', None,
                         FRONT_END_SKIPPED, 'covrec: the Tutorial\'s Power Ups, Attributes and Juice lessons skated (pickups taken)')
ROUTES['cov-traffic'] = ('route', 'boot; career 1; traffic 12; traffic 12 24 0.5; skate 1', None, FRONT_END_SKIPPED,
                         'covrec: the Movie Lot\'s taxis, hits at two speeds')
# The logiccheck and careercheck inputs again (the front end, menus, the
# Tutorial's goals, the pause menu, a save), every call tried, new paths kept.
ROUTES['cov-lc'] = ('pad', 'logiccheck', 11400, {}, 'covrec: the logiccheck input, new paths only')
ROUTES['cov-career'] = ('pad', 'careercheck', 5000, {}, 'covrec: the careercheck input, new paths only')
ROUTES['cov-splitscreen'] = ('route', 'boot; splitscreen; skate 2', None, {'ZX_PAD_PORT1': 'mirror'},
                             'covrec: split screen skated, new paths only')
# Round gamefix (game side): set-ups for the paths the cov- routes still
# missed (tools/routes.py combos, pickuprun, doublegrindenter, spine TURN).
# cov-combos and cov-dgenter are recorded with --new-blocks; the cov-npcs-*
# routes with the classic recorder and only updatePickup (`--passes light
# --only updatePickup`: the list without caps, so the route's environment
# sets them): ZX_FNREC_MIN_CALLS=7 keeps only the calls of an NPC run into
# (the other NPCs' frames make 5 or 6 calls), 3000 calls, 64 a vsync (about
# 20 NPCs are updated a frame).
ROUTES['cov-combos'] = ('route', 'boot; tutorial; combos; combos sf,fm,ms', None, FRONT_END_SKIPPED,
                        'gamefix: the SF, FM and MS grind combos asked for on the rail (jizz kept by a tuning poke)')
for _k, _name in ((1, 'Movie Lot'), (4, 'Boardwalk')):
    ROUTES['cov-npcs-%d' % _k] = ('route', 'boot; career %d; pickuprun 40 300' % _k, None,
                                  dict(FRONT_END_SKIPPED, ZX_FNREC_MIN_CALLS='7', ZX_FNREC_MAX='3000',
                                       ZX_FNREC_PER_VSYNC='64'),
                                  'gamefix: the %s career\'s NPCs run into (updatePickup: knocked back, '
                                  'the get-up)' % _name)
ROUTES['cov-dgenter'] = ('route', 'boot; career 7; doublegrind; doublegrind; ' + '; '.join(
    'doublegrindenter ' + a for a in ('0 0 0 12', '0 0 0 13', '0.3 0', '-0.3 0', '0.95 0', '-0.95 0', '0.95 0 1',
                                      '0 9', '0 9 1', '0 -1', '0 0 0 12 -52.8', '0 0 0 12 -52.4')), None,
    FRONT_END_SKIPPED, 'gamefix: Museum Double Grinds entered with the lean, the short count and a spin set')
# (Grind Balance Best Reset is 0.6 and Reset 0.75: a lean of 0.7 is eased to
# 0.525 and clamped to 0.6; 0.95 is eased to 0.71. A lean of 0.95 made every
# try a plain Grind.)
ROUTES['cov-dgenter2'] = ('route', 'boot; career 7; doublegrind; doublegrind; ' + '; '.join(
    'doublegrindenter ' + a for a in ('0.7 0', '-0.7 0', '0.65 0', '0.7 25', '-0.7 0 0 13', '0.3 0 0 13 -52.8')),
    None, FRONT_END_SKIPPED, 'gamefix: Double Grinds entered with a lean past Grind Balance Best Reset')
ROUTES['cov-spineturn'] = ('route', 'boot; freeskate 0; spine + 14 -31 150 0 L1; spine - 14 -31 150 0 R1; '
                           'spine + 20 -31 150 0 R1; spine - 26 -31 150 0 L1; spine + 26 -31 150 0 L1+R1', None,
                           FRONT_END_SKIPPED, 'gamefix: Movie Lot spines with a spin button held (airSpineTurn\'s turn)')
# Round gamehand (game side): the controls update's analog stick paths
# (tools/routes.py controls), recorded with the classic recorder and --only
# the controls steps (with --new-blocks the steps rested through the short
# presses). Its L1+R1 and R2 presses reach no new block: the pad's pressed
# word (PadState +0x0C) stays 0 for buttons held through ZX_CONTROL.
ROUTES['cov-controls'] = ('route', 'boot; tutorial; controls', None, FRONT_END_SKIPPED,
                          'gamehand: the left stick as an analog stick in and past the dead zone (readStick)')
# Round gamehand3 (game side): soundEvent's (0x1C2558) options and music
# events: the front end's and the pause's sound options (tools/routes.py
# sound: the volume sliders, the playlist's tracks off and on), split screen;
# and play in every career level (mash), recorded with --new-blocks --only
# the sound functions.
ROUTES['cov-sound'] = ('route', 'boot; sound front; freeskate 0; sound pause; skate 1; menu; splitscreen; skate 1; '
                       'sound pause 3; skate 1', None, {'ZX_PAD_PORT1': 'mirror'},
                       'gamehand3: the sound options (volumes, playlist) in the front end, a level and split screen')
ROUTES['cov-sound-play'] = ('route', 'boot; ' + '; menu; '.join('career %d; mash 2400 %d' % (_k, 700 + _k)
                                                                for _k in range(0, 8)), None, {},
                            'gamehand3: play in each career level (mash), for the sound events')
# Round paths1: routes to the swept drafts no route reached (recorded with
# --new-blocks --every-call --only the functions with blocks no recording ran):
# the front end's Options (Save & Load, Sound, Controller, Media Viewer,
# Credits, every cheat code typed and the list), a Timed Run played out twice
# (the statistics, the Timed Game menu), every air trick of the trick table,
# every Multiplayer game to its results, the card dialogs with slot 1 unformatted and slot 2 empty
# (ZX_MC_SLOT1/2). tools/routes.py options, timedrun, timedend, airtricks, mpgame.
ROUTES['cov-frontend'] = ('route', 'boot; options', None, {'ZX_PAD_PORT1': 'mirror'},
                          'paths1: Options (Save & Load, Sound, Controller, Media Viewer, Credits, Cheats: every code)')
ROUTES['cov-timedrun'] = ('route', 'boot; timedrun 3; mash 4200 901; timedend 1; mash 2000 902; timedend 3; '
                          'timedrun 5; mash 3000 903; timedend 3', None, {},
                          'paths1: Timed Run (Boardwalk twice, Cannery) to the end, statistics, the Timed Game menu')
ROUTES['cov-airtricks'] = ('route', 'boot; tutorial; airtricks all; airtricks all -40 SQUARE; airtricks all -40 CIRCLE', None,
                           FRONT_END_SKIPPED, 'paths1: every air trick of the trick table (taps + Square; Circle), '
                           'the air time poked (test only)')
ROUTES['cov-mpgames'] = ('route', 'boot; splitscreen; ' + '; '.join('mpgame %d 300 %d' % (g, 30 + g) for g in (2, 4, 3, 1, 5)),
                         None, {'ZX_PAD_PORT1': 'mirror'},
                         'paths1: split screen, the Multiplayer games (Egg Hunt, Animal Rescue, Twenty One, Best Trick) '
                         'played to their results (a short mash, then UP held to the clock), Sound Options')
ROUTES['cov-cards'] = ('route', 'boot; options nocheats', None, {'ZX_MC_SLOT1': 'unformatted', 'ZX_MC_SLOT2': 'none'},
                       'paths1: the card dialogs with slot 1 unformatted and slot 2 empty (Save & Load)')
ROUTES['cov-devmenu'] = ('route', 'boot; tutorial; devmenu; skate 1', None, dict(FRONT_END_SKIPPED, ZX_DEV_MENU='1'),
                         'paths1: the hidden debug menu (ZX_DEV_MENU=1): Toggle Debug, Respawn, every Tuning '
                         'category, the Art Viewer and its screen dump')
ROUTES['cov-autoload'] = ('route', 'boot; options nocheats; career 1; skate 1', None,
                          {'ZX_ROUTES_CARD': os.path.join(WORKSPACE, 'recomp', 'run', 'mc-sweep-park')},
                          'paths1: a copy of the unlocked card with a saved park (autoload at boot, Save & Load '
                          'over it, a career with the loaded save)')
# The game modes no retail path enters (the mode table 0x24B810: 1 the motion viewer, 4, 7),
# switched to by ZX_GUEST_CALL from the level's update (0x1ACA68) and back to play (2) from
# the mode's own update (test only: it changes what the game does).
_MODE_CALLS = ','.join(['0x1ACA68@4000:0x1A70F0:1', '0x1ABC10@4600:0x1A70F0:2', '0x1ACA68@5000:0x1A70F0:4',
                        '0x1ACC90@5600:0x1A70F0:2', '0x1ACA68@6000:0x1A70F0:7', '0x1C02D8@6600:0x1A70F0:2'])
ROUTES['cov-devmodes'] = ('route', 'boot; tutorial; buttons 6000', None, dict(FRONT_END_SKIPPED, ZX_GUEST_CALL=_MODE_CALLS),
                          'paths1: the developer game modes 1 (motion viewer), 4 and 7 entered from the Tutorial '
                          '(ZX_GUEST_CALL), every button held in turn, back to play')
ROUTES['cov-airtricks2'] = ('route', 'boot; tutorial; airtricks 0,33,34,35,36,37,38,18,39,44,45,47,48,49,51,53,54 -40 SQUARE jizz; '
                            'airchain 60 41; airtricks 0,33,34,35,36,37,38 -60 SQUARE jizz', None, FRONT_END_SKIPPED,
                            'paths1: the special tricks (jizzed, every trick unlocked: test only), chains of air tricks '
                            'in one jump (combine, land, bail)')
ROUTES['cov-pause'] = ('route', 'boot; career 1; pausetour; sound pause 6; menu; career 3; pausetour 1,2,4,5; menu; '
                       'career 6; pausetour 1,4,5', None, {},
                       'paths1: the career pause pages (Challenges and their fly-thrus, Attributes, View Loot, Trick List, '
                       'Save, Sound Options) in the Movie Lot, Industrial and Airfield')
# Round paths2: the Park Editor, tool by tool (tools/routes.py pe2*; ASSETS/EDITOR/INLINE.PEP
# gives the tools: the front end's themes Terrain, Object, Rail and Start; the careers' in-level
# editors Cannery's Pisser, Industrial's Crane and Airfield's Vacuum Tool). Recorded with
# --new-blocks --every-call --only the editor's functions.
ROUTES['cov-pe-tools'] = ('route', 'boot; pe2tools 1; pe2play 1', None, {},
                          'paths2: Park Editor, New Park: Terrain (raise, lower, reselect, delete), all 13 piece groups '
                          '(rotated, placed, refused, deleted), Rail and Start tools, the camera; Test Run (skated, its '
                          'Sound Options, Return to Editor), Sound Options, Clear Park, Save Park (keyboard), Load Park, '
                          'a second Test Run, Save over it, Exit Editor; FREE SKATE > Editor Level with the saved park')
ROUTES['cov-pe-themes'] = ('route', 'boot; pe2enter 0; pe2terrain; pe2objects 1; pe2start; pe2test; pe2exit; '
                           'pe2enter 2; pe2terrain; pe2objects 1; pe2rails; pe2exit; pe2enter 3; pe2objects 1; '
                           'pe2rails; pe2camera; pe2exit', None, {},
                           'paths2: Park Editor in the three other themes: terrain, one piece of each group, rails, '
                           'start, a test run')
ROUTES['cov-pe-park'] = ('route', 'boot; pe2enter 1 load; pe2rails; pe2objects 1; pe2test; pe2load; pe2clear; pe2exit; '
                         'pe2play 2', None, {'ZX_ROUTES_CARD': os.path.join(WORKSPACE, 'recomp', 'run', 'mc-sweep-park')},
                         'paths2: a copy of the card with a saved park (mc-sweep-park): Park Editor > Load Saved '
                         'Park, edited, test run, Load Park, Clear Park, Exit; FREE SKATE > Editor Level with it '
                         '(needs routes.py start\'s ZX_ROUTES_CARD, paths1)')
ROUTES['cov-pe-rails'] = ('route', 'boot; pe2enter 1; pe2railrow 2; pe2test; pe2save; pe2exit; pe2play 1', None, {},
                         'paths2: Park Editor: the groups that take rails, two pieces each, rails placed, changed and '
                         'deleted on each; a test run, saved, played in FREE SKATE')
ROUTES['cov-pe-pieces'] = ('route', 'boot; pe2enter 0; pe2towers; pe2objects 6; pe2railrow 1; pe2test; pe2exit', None, {},
                          'paths2: Park Editor (Prop Yard): pieces on raised and lowered terrain and the terrain '
                          'under pieces (towers), six pieces of each group, rails; a test run')
# Not routed: the careers' in-level editors (Cannery's Pisser, Industrial's Crane, Airfield's
# Vacuum Tool) start only from the level scripts' givers (activateParkEditor 0x1BFE18, the
# project file with the in-level flag): Cannery's "Construct and Score" (the frozen room's
# computer, after Industrial's Cannery Key), Industrial's "Clear the Junkyard" (the junkyard
# machine), Airfield's "Place and Trick on N Items" (the 'B' room's green glow). Calling it by
# ZX_GUEST_CALL from the level's update (0x1ACA68) sets the editor object, then the EE jumps
# to 0xFFFFFFF0 (guest-branch:missing-target; its file loads cannot wait inside a guest call).
# Round paths3: the careers' in-level editors, reached by their givers (tools/routes.py
# keys, giver, tool, dochallenge). On the unlocked card (mc-sweep-unlocked) with the seven
# levels' keys poked into the skater records first (isKeyAwarded: bit level + 9 of the
# record's +8, copied into Player +0x5E8 at level load), as a career with every key has them;
# each giver is the level script's NPC object of that name (its node's position, from the
# scene's name-hash table), talked to with CIRCLE; mash afterwards without teleports.
_UNLOCKED = {'ZX_ROUTES_CARD': os.path.join(WORKSPACE, 'recomp', 'run', 'mc-sweep-unlocked')}
ROUTES['cov-crane'] = ('route', 'boot; keys; career 3; tweaks; giver lvleditor; editing; tool crane 3; dochallenge; '
                       'mash 3600 31 teleport=0', None, _UNLOCKED,
                       'paths3: Industrial "Clear the Junkyard": the junkyard machine (lvleditor), the Crane Tool '
                       '(picks, releases, rotations over a spiral, the camera), Do challenge, play')
ROUTES['cov-pisser'] = ('route', 'boot; keys; career 5; tweaks; giver canneryEditor; editing; tool pisser 3; '
                        'dochallenge; mash 3600 51 teleport=0', None, _UNLOCKED,
                        'paths3: Cannery "Construct and Score": the frozen room\'s computer (canneryEditor), the '
                        'piece dispenser (EditPisserTool: pieces chosen, placed, created, rotated, moved, deleted), '
                        'Do challenge, play')
ROUTES['cov-vacuum'] = ('route', 'boot; keys; career 6; tweaks; giver hangerBEditor; editing; tool vacuum 3; '
                        'dochallenge; mash 5400 61 teleport=0', None, _UNLOCKED,
                        'paths3: Airfield "Place and Trick on 6 Items": the \'B\' room\'s green glow '
                        '(hangerBEditor), the Vacuum Tool (picks, moves, rotations, releases), Do challenge, play '
                        'over the pieces (wait_success: parkEditorAllEditorPiecesTouched)')
ROUTES['cov-parrots'] = ('route', 'boot; keys; career 2; tweaks; giver Chef; mash 2400 21; giver Chef; '
                         'mash 1800 22; giver Chef', None, _UNLOCKED,
                         'paths3: Civic Center "Fetch the Three Parrots to the Chef": the Chef talked to '
                         '(parrotStartGame), play with mash\'s teleports and crashes (a carried parrot dropped), '
                         'the Chef again (parrotHitBase, parrotGetScore)')
ROUTES['cov-construct'] = ('route', 'boot; keys; career 5; tweaks; giver canneryEditor; editing; tool pisser 1; '
                           'dochallenge; bank 60000; boxes; mash 600 52 teleport=0', None, _UNLOCKED,
                           'paths3: Cannery "Construct and Score 50,000" played to its end: the Pisser Tool, Do '
                           'challenge, a 60,000 trick score banked (test only: the pending score poked and landed, '
                           'as `challenge complete`): the challenge complete')
# Round paths4: split screen's games to their results and the pause pages paths1 missed
# (tools/routes.py mpround, pausepages; recorded with --new-blocks --every-call --only the
# features' functions). The Multiplayer menu is the game choice (MultiplayerStart: Most Points,
# Best Trick, Egg Hunt, Twenty One, Animal Rescue); paths1's mpgame chose split screen's pause
# entries instead (Resume, Game Select, Trick List, ...), so no game but Most Points ran.
ROUTES['cov-mpresults'] = ('route', 'boot; splitscreen; mpround 3 900 41 3; mpround 2 900 42 3; mpround 4 900 43 3; '
                           'mpround 1 900 44 3; mpround 0 600 45 2', None, {'ZX_PAD_PORT1': 'mirror'},
                           'paths4: split screen, each Multiplayer game (Twenty One, Egg Hunt, Animal Rescue, Best '
                           'Trick, Most Points) chosen from the Multiplayer menu, played (mash, every live trigger '
                           'zone touched, bails: test only), to the end of its clock, the results; Game Select '
                           'from the pause (the round ended early, its results)')
ROUTES['cov-pausepages'] = ('route', 'boot; career 1; pausepages; menu; career 6; pausepages', None, _UNLOCKED,
                            'paths4: the career pause pages on the unlocked card: every challenge\'s fly-thru '
                            '(CIRCLE), the Summary, a detail, the Trick List\'s six categories with Specials (every '
                            'trick unlocked: test only) in the Movie Lot and Airfield')
# The careers' trigger zones (zones.cpp's list: the props, challenge areas and level objects a
# skater enters), each touched (tools/routes.py zonerun: test-only teleports) on the unlocked
# card with the keys poked (paths3's keys: the key areas open); each level entered twice (the
# second entry restores the level's saved state: FUN_001A50E0); the follow camera's other modes
# (cammode: test only, a poke of its mode byte).
ROUTES['cov-zones-a'] = ('route', 'boot; keys; ' + '; menu; '.join('career %d; tweaks; zonerun all 40; mash 600 %d' % (_k, 70 + _k)
                                                               for _k in (1, 2, 3, 4)) + '; menu; career 4; cammode; skate 1',
                         None, _UNLOCKED,
                         'paths4: every live trigger zone of the Movie Lot, Civic Center, Industrial and Boardwalk '
                         'careers touched (teleports, test only), play; Boardwalk entered again; the follow camera\'s '
                         'modes 1 and 4 (test only)')
ROUTES['cov-zones-b'] = ('route', 'boot; keys; ' + '; menu; '.join('career %d; tweaks; zonerun all 40; mash 600 %d' % (_k, 70 + _k)
                                                               for _k in (5, 6, 7)) + '; menu; career 5; zonerun all 20',
                         None, _UNLOCKED,
                         'paths4: every live trigger zone of the Cannery, Airfield and Museum careers touched '
                         '(teleports, test only), play; Cannery entered again (its saved state restored)')
# The game's own exit (tools/routes.py quit: test only, main's loop flag 0x23FD64 poked, which
# nothing in the retail game sets): main leaves its loop, the game's and the engine's shutdown
# run (FUN_00185D78, FUN_00137D58) and the guest calls Exit(0); from a level and from the front end.
ROUTES['cov-quit'] = ('route', 'boot; career 1; skate 1; quit', None, {},
                      'paths4: the game\'s exit from a career level (main\'s loop flag poked: test only): the '
                      'game and engine shutdown, Exit')
ROUTES['cov-quit-fe'] = ('route', 'boot; quit', None, {},
                         'paths4: the game\'s exit from the main menu (main\'s loop flag poked: test only)')
# The level emitters and the explosion effect (test only: ZX_GUEST_CALL from the play
# mode's update 0x1ACA68 at the level's first frame): every emitter the level script names
# (setEmitterState's hashes in the level's .BIN; most start off and are turned on only by a
# challenge's anim events) turned on by FUN_001C8F00(hash, 1), the native's own callee, and
# FUN_001C8B08's explosion FUN_001CAC70(position, 1) (createExplosion's callee: the explosion
# and its debris) at the skater (g_playerPosition 0x2A76F0); then play.
_EMITTERS = {
    1: '0308d13d 06999f14 0793cc53 07ce1253 08989f35 09a89465 09a899ff 09f1ff8c 0a88b032 0a89eeff 0d1419c0 0d508d24',
    2: '006888e5 00688d7f 0109797f 0307ae02 034b24e5 034b697f 036a4a34 04978432 04a68f34 06703c65 0670d1ff 07980444 '
       '07ff05a4 0945dfd5 097f3c0b 09c0e0e8 0a002427 0a881208 0a882785 0b6815d5 0c0d6585 0c0eae1f 0c556202 0e4a0cf4 '
       '0eaf7a95 0f0c1093',
    4: '00df7735 01fa19a7 097e0a75 0acc5ebf 0c5fdc55 0c70acc4 0c74a2c1 0c74a2c2 0c74a2c3 0c74a2c4 0c74a2c5 0c74a2c6 '
       '0c74a2c7 0c74a2c8 0c74a2c9 0c74a2ca 0f1efd27',
    5: '0217791c 021789d7 04ca35ff 04ca3ba8 04ca4065 057f3dfb 05805134 05d90373 0753d5b5 07f16795 07fefc64 08035384 '
       '080d5ec5 085807a5 091e7fcc 0a349255 0a85de0f 0a85f595 0ca26ae5 0ca270e2',
    6: '00774745 05b6c198 05baa415 067319d5 0bb403b5',
    7: '01ec4e1f 01ec6585 08989f35 098212d5 09821b4f 09a89465 09a899ff 0a528495 0a88aa35 0a88b032 0c4f7775 0ec5eeb2',
}
for _k, _hashes in _EMITTERS.items():
    _calls = ','.join(['0x1ACA68@1:0x1C8F00:0x%s:1' % _h for _h in _hashes.split()] + ['0x1ACA68@1:0x1CAC70:0x2A76F0:1'])
    ROUTES['cov-emitters-%d' % _k] = ('route', 'boot; career %d; tweaks; skate 2; mash 900 %d' % (_k, 80 + _k), None,
                                      dict(_UNLOCKED, ZX_GUEST_CALL=_calls),
                                      'paths4: career level-select entry %d with every emitter of its script turned '
                                      'on and an explosion at the skater (test only: ZX_GUEST_CALL), skated' % _k)
# The splash effect (createSplash's callee FUN_001CB980(position, &(size, speed), f12); test only:
# ZX_GUEST_CALL at the skater with the pair 0.5, 0 at 0x269D40) in the Boardwalk, at the level's
# first frame and again later.
ROUTES['cov-splash'] = ('route', 'boot; career 4; tweaks; skate 2; mash 600 91', None,
                        dict(_UNLOCKED, ZX_GUEST_CALL='0x1ACA68@1:0x1CB980:0x2A76F0:0x269D40,'
                                                        '0x1ACA68@4000:0x1CB980:0x2A76F0:0x269D40'),
                        'paths4: the splash effect at the skater (test only: ZX_GUEST_CALL) in the Boardwalk, skated')
# zfile's watcher (watchFileOperation 0x13EFD0: on only while 0x273A98 is set, which nothing in
# the game does; test only: poked after boot), then a level loaded and skated.
ROUTES['cov-filewatch'] = ('route', 'boot; poke 0x273A98 1; career 1; skate 1; menu; freeskate 0; skate 1', None, {},
                           'paths4: zfile\'s file-operation watcher switched on (test only: a poke), a career level '
                           'and a free skate loaded and skated')
# Round paths5: code no shipped data reaches, run by test-only guest calls (ZX_GUEST_CALL, with
# ZX_READABLE=off as every recording), and the careers' editor pieces ridden.
# The ragdoll: every player loads assets\dynamics\edwards.zad (FUN_001A7160: 10 bodies, 9
# constraints; Player +0x0C), the only ZDYN on the disc with constraints, but its stepper FUN_001A4968
# (player, frame time) has no caller (a cut ragdoll crash), nor is the ragdoll ever attached to the
# skater. Before groundMove (0x19B118: a0 the player, f12 the frame time) every 20 vsyncs: every
# 10th time wakeDynamics (0x159AC0: the skater's scene object 0x72FDA0, the ragdoll 0x970A50: its
# bodies posed on the skater's bones), else the stepper with player one (0x974C00); the addresses
# are this route's (Airfield from boot on the unlocked card; `player` prints them). Unattached, the
# stepper writes through bone 0 into low memory and the game fails a few calls later.
_RAGDOLL = ','.join(('0x19B118@%d:0x159AC0:0x72FDA0:0x970A50' if _k % 10 == 0 else '0x19B118@%d:0x1A4968:0x974C00')
                    % (3200 + 20 * _k) for _k in range(150))
ROUTES['cov-ragdoll'] = ('route', 'boot; career 6; player; states 1500; tweaks; skate 1', None,
                         dict(_UNLOCKED, ZX_GUEST_CALL=_RAGDOLL),
                         'paths5: Airfield, the players\' ragdoll (edwards.zad) posed on the standing skater and '
                         'stepped by its stepper (test only: ZX_GUEST_CALL of wakeDynamics and the uncalled '
                         'FUN_001A4968), then skated')
# The lens flare (fx init sets a sun position for the Movie Lot, Boardwalk and Industrial but never
# starts weather kinds 5 and 6: FUN_001C8B08(6) by guest call), then one particle of each effect class
# only particle emitter objects (none on the disc) spawn: bubble, firefly, bloodpool, screenshard
# (their creators with the splash's default parameters 0x255070) and splash (its own default), at the
# skater (0x2A76F0).
_FXCLASSES = ','.join(['0x1ACA68@3300:0x1C8B08:6'] +
                      ['0x1ACA68@%d:%#X:0x2A76F0:0x255070' % (3400 + 100 * _i, _t)
                       for _i, _t in enumerate((0x1CF6C8, 0x1D0230, 0x1D2090, 0x1D5010))] +
                      ['0x1ACA68@3800:0x1D5700:0x2A76F0:0', '0x1ACA68@5000:0x1CF6C8:0x2A76F0:0x255070',
                       '0x1ACA68@5100:0x1D0230:0x2A76F0:0x255070', '0x1ACA68@5200:0x1D2090:0x2A76F0:0x255070',
                       '0x1ACA68@5300:0x1D5700:0x2A76F0:0'])
ROUTES['cov-fxclasses'] = ('route', 'boot; career 1; tweaks; skate 2; mash 900 94', None,
                           dict(_UNLOCKED, ZX_GUEST_CALL=_FXCLASSES),
                           'paths5: the Movie Lot\'s lens flare and the effect classes no shipped emitter '
                           'spawns (test only: ZX_GUEST_CALL), skated')
# Airfield's "Place and Trick on 6 Items": the Vacuum Tool, Do challenge, each placed piece ridden into
# from its four sides (piecetouch: skaterEffects calls FUN_001C6728 for a piece ridden); the
# Tutorial left by the pause menu (level 7's teardown FUN_001C7DB8).
ROUTES['cov-pieces'] = ('route', 'boot; keys; career 6; tweaks; giver hangerBEditor; editing; tool vacuum 1; '
                        'dochallenge; pieces; piecetouch; piecetouch 12 90; mash 600 95 teleport=0; menu; '
                        'tutorial; skate 1; menu', None, _UNLOCKED,
                        'paths5: Airfield\'s editor pieces placed (the Vacuum Tool) and ridden into, then the '
                        'Tutorial skated and left')
# The input recorder (replayFrame 0x1D9CF8) runs only after setReplayMode 0x1D9908 (1 record, 2 play,
# 3 stop, 0 off), which only the uncalled debug toggle FUN_001DACA8 calls. Recording's start saves the
# players' game objects (FUN_00195A28 sizes, FUN_00195A90 saves: each object's size and save messages,
# so the trick objects' handlers of the kinds active then), playback's start restores them (FUN_00195C90:
# new objects, their load messages). By guest call (test only) in a free skate under mash: a start and
# an off every 25 vsyncs (80 starts in different moves), then record, stop, play, off.
_REPLAY = ','.join(['0x1D9CF8@%d:0x1D9908:%d' % (3200 + 25 * _k, (_k + 1) % 2) for _k in range(80)] +
                   ['0x1D9CF8@5300:0x1D9908:1', '0x1D9CF8@5350:0x1D9908:3', '0x1D9CF8@5360:0x1D9908:2',
                    '0x1D9CF8@5500:0x1D9908:0'])
# The game's own CD client (ZX_DISC=ps2: the runtime's disc layer off, the original's file device
# and sceCd calls run): boot, a career level and a free skate loaded and skated.
ROUTES['cov-cdvd'] = ('route', 'boot; career 1; skate 1; menu; freeskate 0; skate 1', None,
                      dict(_UNLOCKED, ZX_DISC='ps2'),
                      'paths5: the files read through the game\'s CD client (ZX_DISC=ps2): boot, a career level '
                      'and a free skate')
ROUTES['cov-replay'] = ('route', 'boot; freeskate 0; tweaks; mash 6000 96', None,
                        dict(_UNLOCKED, ZX_GUEST_CALL=_REPLAY),
                        'paths5: the Movie Lot\'s free skate under mash with the input recorder started and '
                        'stopped (test only: ZX_GUEST_CALL of setReplayMode), then a playback')
# Longer, denser: a start and an off 12 vsyncs apart every 25 vsyncs, and every 600 vsyncs a record,
# stop (50 vsyncs on: a stop or playback sooner hung the game in tests), playback and off; Movie Lot
# only (a recording start hung the Boardwalk's free skate).
_REPLAY2 = []
_t = 3300
while _t < 8300:
    if (_t - 3300) % 600 == 0 and _t > 3300:
        _REPLAY2 += ['0x1D9CF8@%d:0x1D9908:%d' % (_t + _o, _m) for _o, _m in ((0, 1), (50, 3), (60, 2), (200, 0))]
        _t += 225
        continue
    _REPLAY2 += ['0x1D9CF8@%d:0x1D9908:1' % _t, '0x1D9CF8@%d:0x1D9908:0' % (_t + 12)]
    _t += 25
ROUTES['cov-replay2'] = ('route', 'boot; freeskate 0; tweaks; mash 12000 98', None,
                         dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_REPLAY2)),
                         'paths5: as cov-replay, longer, with a playback every 600 vsyncs (test only)')
# Round paths6. The movies through the game's own ps2Movie stream and sound code (ZX_MOVIES=hle:
# the runtime's host movie player off): the careercheck input from boot to vsync 600, the first logo
# movie (the HLE then stalls in GetPicture from about vsync 185: drafter23, the audio ring is never
# drained).
ROUTES['cov-movies'] = ('pad', 'careercheck', 600, {'ZX_MOVIES': 'hle'},
                        'paths6: boot with the game\'s own movie stream code (ZX_MOVIES=hle), the first logo movie')
# The weather (snow: the Civic Center; rain and lightning: Airfield) suspended and resumed as the level
# scripts' suspendWeatherEffects / resumeWeatherEffects natives do (FUN_001C8EC0 writes 0x2796C8 and
# 0x2796C4 = 1, FUN_001C8ED0 0x2796C4 = 0: here by poke), the particle budget (fx init: 0x273DD8 =
# 0x200) poked to 0 for a while (every spawner's budget-full path) and back, the game paused for 300
# vsyncs with particles alive (test only: pokes).
_WEATHER = ('tweaks; skate 1; poke 0x2796C8 1 0x2796C4 1; wait 120; skate 1; poke 0x2796C4 0; skate 1; '
            'poke 0x273DD8 0; skate 1; wait 300; poke 0x273DD8 0x200; skate 1; pausefor 300; skate 1')
for _k, _name in ((2, 'the Civic Center (snow)'), (6, 'Airfield (rain, lightning)')):
    ROUTES['cov-weather-%d' % _k] = ('route', 'boot; career %d; %s' % (_k, _WEATHER), None, _UNLOCKED,
                                     'paths6: %s: the weather suspended and resumed, the particle budget '
                                     'full, a pause (test only: pokes)' % _name)
# The ragdoll solved (paths5's cov-ragdoll stepped it through FUN_001A4968 with a player address of
# another build: its system had no articulation, so the solver's loops never ran). Here the system
# itself (Player +0x0C, 0x970A50 in Airfield from boot: 10 bodies, 9 constraints, the articulation
# fn_153688 built at load: +0x20 the root joint, +0x2C 10, +0x30 9) is stepped by updateDynamics
# (0x15B1B8: a0 the system, a1 0, f12 the frame time, as groundMove 0x19B118 has it) every vsync for
# 400 vsyncs after each wakeDynamics (0x159AC0: the skater's scene object 0x72FDA0, the system: its
# bodies posed on the skater's bones), three times while skating (test only: ZX_GUEST_CALL).
_RAGDOLL2 = []
for _w in (4000, 4700, 5400):
    _RAGDOLL2 += ['0x19B118@%d:0x159AC0:0x72FDA0:0x970A50' % _w]
    _RAGDOLL2 += ['0x19B118@%d:0x15B1B8:0x970A50:0' % _v for _v in range(_w + 1, _w + 400)]
ROUTES['cov-ragdoll2'] = ('route', 'boot; career 6; player; tweaks; skate 3; mash 2400 97; player', None,
                          dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_RAGDOLL2)),
                          'paths6: Airfield, the players\' ragdoll (edwards.zad) posed on the skater and solved by '
                          'updateDynamics every vsync (test only: ZX_GUEST_CALL), skated')
# What the game's files never give the solver (zgehand20, test only; the addresses as cov-ragdoll2's):
# the ragdoll system stepped by updateDynamics as above, with a hold on its first bodies
# (newBodyContactRecord 0x15BB30, dead in the game: the point at the body's box corner held at the
# position it wakes at; one more then switched off through its strength, one of another kind), so
# resolveBodyContactList and resolveBodyContact run; then its links cut (+0x14 cleared), which steps
# each body by itself, and the first body's probe radius (its extents' length) made 30, so
# collisionQuery hands findBodyFaceContacts more than the 40 faces of the world table.
_HOLD = 0x970A50
_BODY0 = '%%*(%#x+0x10)' % _HOLD
_BODY1 = '%%*(*(%#x+0x10)+0x3D8)' % _HOLD
_CONTACTS = []
for _w in (4000, 4700):
    _CONTACTS += ['0x19B118@%d:0x159AC0:0x72FDA0:%#x' % (_w, _HOLD)]
_CONTACTS += ['0x19B118@4001:~0x15BB30:%s:#16:%%(*(%#x+0x10)+0xE0):0' % (_BODY0, _HOLD),
              '0x19B118@+1:~0x15BB30:%s:#16:%%(*(*(%#x+0x10)+0x3D8)+0xE0):0' % (_BODY1, _HOLD)]
# Switched off (strength 0), and of no kind the list knows, on two more records of the second body.
_CONTACTS += ['0x19B118@+1:~0x15BB30:%s:#16:%%(*(*(%#x+0x10)+0x3D8)+0xE0):0' % (_BODY1, _HOLD),
              '0x19B118@+1:W:%%(*(*(*(%#x+0x10)+0x3D8)+0x388)+8):0' % _HOLD,
              '0x19B118@+1:~0x15BB30:%s:#16:%%(*(*(%#x+0x10)+0x3D8)+0xE0):0' % (_BODY1, _HOLD),
              '0x19B118@+1:W:%%*(*(*(%#x+0x10)+0x3D8)+0x388):1' % _HOLD]
_CONTACTS = _CONTACTS[:1] + _CONTACTS[2:] + _CONTACTS[1:2]
_CONTACTS += ['0x19B118@%d:0x15B1B8:%#x:0' % (_v, _HOLD) for _v in range(4010, 4400)]
_CONTACTS += ['0x19B118@%d:W:%#x:0' % (4700 - 1, _HOLD + 0x14),
              '0x19B118@+1:W:%%(*(*(%#x+0x10)+0x3D4)+0x10):0x41F00000' % _HOLD]
_CONTACTS += ['0x19B118@%d:0x15B1B8:%#x:0' % (_v, _HOLD) for _v in range(4701, 4900)]
ROUTES['cov-zgh20-bodies'] = ('route', 'boot; career 6; player; tweaks; skate 3; mash 2400 97; player', None,
                              dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_CONTACTS)),
                              'zgehand20: Airfield, the ragdoll solved with holds on its bodies, then stepped body by '
                              'body with a large probe: more than 40 faces to test (test only)')
# The emitter-only effect classes again (paths5's cov-fxclasses spawned each once, while skating): every
# 40 vsyncs one of them at the skater (0x2A76F0) while it stands (the camera on it), so their draws run:
# bubble 0x1CF6C8, firefly 0x1D0230, bloodpool 0x1D2090, screenshard 0x1D5010 (the splash's default
# parameters 0x255070) and 0x1D5700 (its own default), test only (ZX_GUEST_CALL).
_FXCLASSES2 = ['0x1ACA68@%d:%#X:0x2A76F0:%s' % (3400 + 40 * _i, _t, '0' if _t == 0x1D5700 else '0x255070')
               for _i, _t in enumerate([0x1CF6C8, 0x1D0230, 0x1D2090, 0x1D5010, 0x1D5700] * 12)]
ROUTES['cov-fxclasses2'] = ('route', 'boot; career 1; tweaks; states 3000; skate 1', None,
                            dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_FXCLASSES2)),
                            'paths6: the emitter-only effect classes spawned at the standing skater again and again '
                            '(test only: ZX_GUEST_CALL), then skated')
# The texture cache's overflow recovery (FUN_00107AB8, from advanceTextureCache when low passes high,
# which the flush under 384 KB free otherwise keeps from happening): the overflow flag (0x27A954) and a
# small free space (0x27A950) poked in a level, test only.
ROUTES['cov-texcache'] = ('route', 'boot; career 1; skate 1; poke 0x27A954 1; wait 30; skate 1; poke 0x27A950 0x1000; '
                          'wait 30; skate 1; menu; freeskate 4; poke 0x27A954 1; skate 1', None, {},
                          'paths6: the texture cache overflow recovery and its flush request (test only: pokes)')
# The level scripts' anim objects and challenge objects sent their syncStart, play and objectiveComplete
# events (test only: ZX_GUEST_CALL of scriptSendEvent, FUN_001E05B0: a0 the object's name hash, a1 the
# sender, a2 the event's), one every 200 vsyncs from vsync 3500, from the play mode's update (0x1ACA68),
# while mash plays: what a challenge's completion sets off (animations and their anim events: traffic
# hidden and shown, explosions, the weather suspended, audio syncs). The objects and events are those of
# each level script (aggressive_script_dis.py; names from the .LOG, else the hashes the script has).
def _name_hash(s):
    if s.startswith('0x'):
        return int(s, 16)
    h = 0
    for c in s.upper().encode('latin-1'):
        h = ((h << 4) + c) & 0xFFFFFFFF
        g = h & 0xF0000000
        if g:
            h ^= g >> 24
        h &= ~g & 0xFFFFFFFF
    return h


_SYNCS = {
    1: ('AnimObj_BoulderPartOne:syncStart AnimObj_WickedGround:syncStart AnimObj_WickedGround:play '
        'AnimObj_BoulderPartTwo:syncStart AnimObj_BoulderPartTwo:play scriptObj_Switch05A:syncStart '
        'scriptObj_Switch05A:play'),
    2: ('ScriptObj_Wings:syncStart ScriptProp_gargoyle1:syncStart ScriptProp_gargoyle2:syncStart '
        'AnimObj_MarkGlobe:syncStart AnimObj_MarkGlobe:play animObj_Clock:play animObj_ClockGears:play '
        'animObj_ClockTower:syncStart animObj_ClockTower:play animProp_BreakingTheLaw1:syncStart '
        'animProp_BreakingTheLaw1:play animProp_BreakingTheLaw2:play animProp_BreakingTheLaw3:play '
        'animProp_BreakingTheLaw4:play animProp_BreakingTheLaw5:play animObj_ScalesJustice:syncStart '
        'scriptObj_venusBefore:syncStart ScriptObj_htlgutter:syncStart ScriptObj_htlgutter:play'),
    3: '0x0b7cc07b:syncStart 0x0b7cc07b:play 0x05a6d8f0:syncStart',
    4: ('0x01484443:syncStart 0x01484443:play 0x026a302c:play 0x0c63d4f3:play 0x01f58e52:syncStart '
        '0x0bd04ec5:syncStart 0x06b22be3:syncStart 0x0835e3c2:syncStart 0x04ba08fc:syncStart'),
    5: ('AnimObj_Boats:syncStart AnimObj_Boats:play scriptObj_propanetrig01:objectiveComplete '
        'scriptObj_propanetrig01:syncStart animObj_FireHarpoon:syncStart animObj_FireHarpoon:play '
        'animObj_Swordfish:syncStart AnimObj_ControlShovel:syncStart scriptObj_tunaconveyor05:objectiveComplete'),
    6: ('AnimObj_AmeliaSkitch:play AnimObj_Snail:syncStart AnimObj_Snail:play AnimObj_Seahorse:syncStart '
        'AnimObj_Seahorse:play AnimObj_StrayDrop:syncStart AnimObj_StrayDrop:play animobj_StrayMissile01:play '
        'AnimObj_LunerModule:syncStart'),
    7: 'ScriptObj_Iceblock:syncStart ScriptObj_Backbone:syncStart animObj_FireThe_tank:syncStart animObj_TheEye:syncStart',
}
for _k, _pairs in _SYNCS.items():
    _calls = ['0x1ACA68@%d:0x1E05B0:%#x:0:%#x' % (3500 + 200 * _i, _name_hash(_o), _name_hash(_e))
              for _i, (_o, _e) in enumerate(_p.split(':') for _p in _pairs.split())]
    ROUTES['cov-syncs-%d' % _k] = ('route', 'boot; career %d; tweaks; mash %d %d' % (_k, 400 + 200 * len(_calls) + 1200, 110 + _k),
                                   None, dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_calls)),
                                   'paths6: career level %d, the script objects sent their syncStart, play and '
                                   'objectiveComplete events (test only: ZX_GUEST_CALL of scriptSendEvent), '
                                   'under mash' % _k)
# A card too full for a save (routes.py cardfill: an 8,000 KB file of its own on slot 1's card):
# Options > Save & Load, then a career's pause > Save.
ROUTES['cov-cardfull'] = ('route', 'boot; cardfill 8000; options nocheats; career 1; pausetour 3; menu; '
                          'cardfill 0; career 1; pausetour 3', None, {},
                          'paths6: the card in slot 1 too full for a save: Save & Load and the pause menu\'s '
                          'Save, then with room again')


# Earlier routes recorded again on drafter23's recorder (calls that wait kept: the card worker, boot and
# level-load functions, main on the exit routes), into folders of their own (`-w`): the same play.
for _r in ('career', 'cov-autoload', 'cov-cards', 'cov-devmenu', 'cov-frontend', 'cov-mpresults', 'cov-pause',
           'cov-pausepages', 'cov-pe-park', 'cov-pe-rails', 'cov-quit-fe', 'cov-timedrun', 'cov-pe-tools',
           'cov-pe-themes', 'cov-pe-pieces', 'cov-crane', 'cov-pisser', 'cov-vacuum', 'cov-lc', 'cov-parrots', 'cov-career'):
    ROUTES[_r + '-w'] = ROUTES[_r][:4] + ('paths6: %s again with calls that wait kept (%s)' % (_r, ROUTES[_r][4]),)


# Round paths7. zfile's pcfs device (FUN_0011A5E0 registers dvd0, pcfs, mc00 and, after it, rd00; a path
# names its device by prefix, "pcfs:...", else the default device 0x2991F8, which zfile's init sets to dvd0
# 0x23E380): the default poked to pcfs's method table 0x2347B0 from the main menu on (test only), so every
# file the game opens goes through pcfs's own open, read, seek, size and close (rwfopen: sceOpen of
# "cdrom0:\PATH;1", the path upper-cased with '/' made '\'), levels loaded and left.
ROUTES['cov-pcfs'] = ('route', 'boot; poke 0x2991F8 0x2347B0; career 1; skate 1; menu; freeskate 0; skate 1; menu; '
                      'freeskate 4; skate 1', None, {},
                      'paths7: every file opened through zfile\'s pcfs device (the default device poked: test only), '
                      'three levels loaded, skated, left')
# zgui's asynchronous library load: LOADLIB with a third word (the script to run once loaded) reads the
# library through zfile's watcher task (watchTaskFunction 0x13EC28: the queued seek and read, the seek's
# completion 0x140B10); only DM2's menus and AI's leftover MENU/MAIN.ZSC ask for it, AI's UILIB.ZAL never
# (loadlib.py). Here loadZguiLibrary (0x17EF28: name, path, completion) by guest call with strings
# (ZX_GUEST_STRINGS, test only): in the front end and again in a level.
ROUTES['cov-asynclib'] = ('route', 'boot; wait 1500; career 1; skate 2', None,
                          {'ZX_GUEST_STRINGS': 'P7Front|uilib:PauseMenu.Zif|LoadComplete|P7Level|uilib:SaveLoad.Zif',
                           'ZX_GUEST_CALL': '0x100968@1500:0x17EF28:$0:$1:$2,0x100968@5000:0x17EF28:$3:$4:$2'},
                          'paths7: zgui libraries loaded asynchronously (LOADLIB\'s completion form: test only, guest '
                          'calls), in the front end and in the Movie Lot')
# The explosions paths6's cov-syncs sent events for that never went off: each one's script object handles
# syncStart only in its state `active`, and was still in `init` (aggressive_script_dis.py with the Xbox
# release's .LOG names): Boardwalk's AnimObj_Boulders (set active by DamAssigner's syncStart, that giver
# needing key 0), Airfield's AnimObj_LunerModule (active after two getReady events: scriptobj_SWballoonD's
# switchStall and scriptObj_daveHelper07's glassBreak; its syncStart plays AnimObj_SnailExplode's
# animation, whose anim events explode), the Museum's animObj_FireThe_tank (set active by bankRobber's
# action). Here the state set (setState 0x1E0798: object hash, state hash) or the events sent
# (scriptSendEvent 0x1E05B0), then syncStart, under mash (test only: ZX_GUEST_CALL).
_ACTIVE, _SYNC, _READY = 0x04588EA5, 0x0288DE94, 0x0A969129
_EXPLODE = {
    4: [(3500, 0x1E0798, (0x06B22BE3, _ACTIVE)), (3600, 0x1E05B0, (0x06B22BE3, 0, _SYNC))],
    6: [(3500, 0x1E05B0, (0x0D4C5985, 0, _READY)), (3520, 0x1E05B0, (0x0D4C5985, 0, _READY)),
        (3600, 0x1E05B0, (0x0D4C5985, 0, _SYNC))],
    7: [(3500, 0x1E0798, (0x0DF8DE0B, _ACTIVE)), (3600, 0x1E05B0, (0x0DF8DE0B, 0, _SYNC))],
}
for _k, _calls in _EXPLODE.items():
    ROUTES['cov-explode-%d' % _k] = (
        'route', 'boot; career %d; tweaks; mash 4000 %d' % (_k, 120 + _k), None,
        dict(ZX_GUEST_CALL=','.join('0x1ACA68@%d:0x%X:%s' % (_v, _t, ':'.join('%#x' % _a for _a in _a3))
                                      for _v, _t, _a3 in _calls)),
        'paths7: career level-select entry %d: an explosion its level script holds back until a challenge sets '
        'the object active (test only: the state set or the events sent by guest call), under mash' % _k)
# Every skater of the select screen in free skate, the level left each time: skaters whose model has the
# bones "Breast Lft" or "Ponytail Lft 1" / "Ponytail Rt 1" get their secondary-motion objects at set-up
# (FUN_001910B8: Player +0x904, +0x910, +0x91C), freed at the level's teardown (FUN_00193CE8 ->
# FUN_001462E8 and its six frees).
ROUTES['cov-skaters'] = ('route', 'boot; ' + '; '.join('freeskate 1 %d; states 200; menu; mainmenu' % _s for _s in range(16)),
                         None, {}, 'paths7: free skate with each of the 16 skaters, the level left each time')
# The engine's pad recorder (0x29A150: pollPads records each pad's frame into a 64 KB buffer and flushes it
# to a file, or plays one back; FUN_0014C768(path, 1 record / 2 play) starts it and nothing calls it; the
# stop flag 0x29A158 makes readPads flush and close): started recording in the Movie Lot by guest call onto a
# ram-disk file over commandLoadLib's own buffer (rd00:0x2AA6F8:0x100: ramDiskWrite keeps a write inside the
# file), stopped (poke), played back, stopped again (test only).
ROUTES['cov-padrec'] = ('route', 'boot; career 1; tweaks; mash 2000 140; poke 0x29A158 1; wait 60; mash 1500 141; '
                        'poke 0x29A158 1; wait 60; skate 1', None,
                        {'ZX_GUEST_STRINGS': 'rd00:0x2AA6F8:0x100',
                         'ZX_GUEST_CALL': '0x1ACA68@3500:0x14C768:$0:1,0x1ACA68@5600:0x14C768:$0:2'},
                        'paths7: the engine\'s pad recorder recording and playing back in the Movie Lot (test only: '
                        'guest calls, pokes)')
# The level objects' engine messages no route sent (they come from animation events, not script SENDs:
# paths5): an object found by its name hash (findObjects 0x1646B8: `=V` a pointer to the hash, `*^N` the first
# object of the list it returns), then sendMessage (0x1528D8: 0, object, event, data, synchronous) of 0x7002,
# ListenForActions, SOUND_EVENT and play to the Movie Lot's NPCs kelly and madScientist and its
# AnimObj_BoulderPartOne (a sound-event object), under mash (test only: guest calls).
_NPCMSG = []
for _i, _o in enumerate((0x004FA119, 0x089619F4, 0x02D5B9C5)):
    _b = len(_NPCMSG)
    _NPCMSG += ['0x1ACA68@%d:0x1646B8:=0x%x:1' % (3500 + 10 * _i, _o)]
    _NPCMSG += ['0x1ACA68@%d:0x1528D8:0:*^%d:0x%x:#64:1' % (3500 + 10 * _i, _b, _e)
                for _e in (0x7002, 0x0D2F7AD3, 0x0A62E414, 0x00055069)]
ROUTES['cov-npcmsg'] = ('route', 'boot; career 1; tweaks; mash 2000 142', None, {'ZX_GUEST_CALL': ','.join(_NPCMSG)},
                        'paths7: the Movie Lot\'s NPCs and a sound-event object sent the engine messages animation '
                        'events send (test only: guest calls), under mash')
# The editors' button callbacks no route pressed (each tool registers its buttons with FUN_001F9580: the Rail
# Tool's Cross, the Start Tool's Triangle and Square, the Vacuum Tool's Triangle, ...): the Park Editor with a
# row of rail pieces, each button pressed in the Rail, Start, first piece group and Terrain tools; Airfield's
# in-level editor with the Vacuum Tool's buttons (tools/routes.py buttons7: paths7).
ROUTES['cov-pebuttons'] = ('route', 'boot; pe2enter 1; pe2railrow 1; buttons7 TRIANGLE,SQUARE,CROSS,LEFT,L3,CIRCLE 14,15,1,0; '
                           'pe2exit', None, {},
                           'paths7: the Park Editor\'s tools with every button pressed')
ROUTES['cov-vacbuttons'] = ('route', 'boot; keys; career 6; tweaks; giver hangerBEditor; editing; tool vacuum 1; '
                            'buttons7 TRIANGLE,SQUARE,CIRCLE,CROSS,LEFT,UP,L3', None, ROUTES['cov-vacuum'][3],
                            'paths7: Airfield\'s in-level editor, the Vacuum Tool with every button pressed')
# The same Park Editor with the buttons paths7's route left out (L2, R2, SELECT, START, R3 and the d-pad's other
# directions): the Rail Tool's previous and next rail (and fitting rail) handlers, the first-tool button, the
# Object Tool's previous piece.
ROUTES['cov-pebuttons2'] = ('route', 'boot; pe2enter 1; pe2railrow 1; buttons7 L2,R2,SELECT,START,R3,UP,DOWN,RIGHT 14,15,1,0; '
                            'pe2exit', None, {},
                            'paths8: the Park Editor\'s tools with the buttons paths7 left out pressed')
# Cannery's in-level editor, the piece dispenser (Pisser Tool) with a piece taken (CROSS) and then every button
# pressed in turn: its move right, move forward and rotate handlers.
ROUTES['cov-pisserbuttons2'] = ('route', 'boot; keys; career 5; tweaks; giver canneryEditor; editing; tool pisser 1; '
                                'buttons7 CROSS,UP,DOWN,LEFT,RIGHT,L2,R2,CIRCLE,SQUARE,TRIANGLE,SELECT,START,L3,R3', None,
                                _UNLOCKED, 'paths8: the Pisser Tool with a piece taken and every button pressed')
# The Park Editor's Start and Object Tools with a piece taken (CROSS) and the d-pad pressed after it: the Start Tool's
# move left and back, the Object Tool's move back.
ROUTES['cov-pebuttons3'] = ('route', 'boot; pe2enter 1; buttons7 CROSS,LEFT,DOWN,UP,RIGHT,CIRCLE,SQUARE,TRIANGLE 15,1,0,14; pe2exit',
                            None, {}, 'paths8: the Park Editor\'s tools with a piece taken and the d-pad pressed after it')
# Text trick objects (kind 5: a goal's or a script's bonus "trick" with a name and points: tellGoalsOver and the
# native addBonusTrickToTrickQueue make them, and no shipped level fills the goal list or calls the native):
# newTextObject (0x1EB2D0: text, value) and the trick queue's add (0x1B3A00: player one's record, read as
# `%*(*0x24C9BC+0x10)+0x10`, the object) by guest call in the Movie Lot, one of them while the input
# recorder starts and plays back (its objects saved and restored), under mash (test only).
_P1 = '%*(*0x24C9BC+0x10)+0x10'
_TEXTOBJ = []
for _i, _v in enumerate((3500, 3800, 4100)):
    _TEXTOBJ += ['0x1ACA68@%d:0x1EB2D0:$%d:%d' % (_v, _i % 2, _i), '0x1ACA68@%d:0x1B3A00:%s:^%d' % (_v, _P1, len(_TEXTOBJ))]
_TEXTOBJ += ['0x1ACA68@+2:0x1D9908:1', '0x1D9CF8@+50:0x1D9908:3', '0x1D9CF8@+10:0x1D9908:2', '0x1D9CF8@+140:0x1D9908:0']
ROUTES['cov-textobj'] = ('route', 'boot; freeskate 0; tweaks; mash 2500 143', None,
                         dict(_UNLOCKED, ZX_GUEST_STRINGS='P7 Gap Bonus|P7 Second', ZX_GUEST_CALL=','.join(_TEXTOBJ)),
                         'paths7: text trick objects put in the trick queue by guest call, the input recorder started '
                         'and played back with one in it (test only), free skate under mash')
# AI's leftover MENU/PARK.ZSC (a DM2-era park menu with a guiLine) by LOADZSC, and a menu script of our own
# (a guiIdleFunc, a guiLine and an Animate action, the types only DM2's menus and AI's leftovers define) read
# from a ram-disk file over a heap block: malloc 0x138008, strcpy 0x148638, sprintf 0x148760 of its
# rd00:ADDR:SIZE path, the script loader 0x171170 (LOADZSC's, past its upper-casing, which would hide rd00),
# in the front end (test only: guest calls).
_ZSC = ('OBJECT "guiScreenSpace" "P7Root"\r\n{\r\n\tOnCreate\t"PLAY P7Anim"\r\n\tVisible\t1\r\n'
        '\tScale\t1.000000 1.000000 1.000000\r\n\tOBJECT "guiIdleFunc" "P7Idle"\r\n\t{\r\n\t\tVisible\t1\r\n'
        '\t\tScale\t1.000000 1.000000 1.000000\r\n\t\tActivationFrame\t2\r\n\t\tActivateOnce\t0\r\n\t}\r\n'
        '\tOBJECT "guiLine" "P7Line"\r\n\t{\r\n\t\tVisible\t1\r\n\t\tPosition\t20.000000 60.000000 0.000000\r\n'
        '\t\tScale\t1.000000 1.000000 1.000000\r\n\t\tVector\t470.000000 10.000000 0.000000\r\n\t\tWidth\t2\r\n'
        '\t}\r\n\tACTION "Animate" "P7Anim"\r\n\t{\r\n\t\tStartDelay\t0\r\n'
        '\t\tOnUpdate\t"ROTQUATY Orientation {0.005}"\r\n\t\tLoopMode\t1\r\n\t\tLoopTime\t0\r\n\t\tSpeed\t1\r\n'
        '\t}\r\n}\r\n')
ROUTES['cov-zguizsc'] = ('route', 'boot; wait 2400', None,
                         {'ZX_GUEST_STRINGS': '|'.join((_ZSC, 'rd00:%#x:%#x', 'LOADZSC assets\\menu\\park.zsc')),
                          'ZX_GUEST_CALL': ','.join(('0x100968@1500:0x138008:0x800', '0x100968@1500:0x138008:0x40',
                                                       '0x100968@1500:0x148638:^0:$0',
                                                       '0x100968@1500:0x148760:^1:$1:^0:%d' % len(_ZSC),
                                                       '0x100968@1500:0x171170:^1:%*0x240A1C',
                                                       '0x100968@1600:0x1820F8:$2'))},
                         'paths7: AI\'s leftover PARK.ZSC and a menu script of our own with the types only other '
                         'scripts define, loaded in the front end (test only: guest calls)')
# The engine's debug geometry (drawDebugGeometry 0x14B430 draws a list of five primitive kinds; the adders
# 0x14B870 (a matrix), 0x14B8E8, drawTriangle 0x14B9B0, 0x14BA58 and 0x14BAE0 add only while 0x299B24 is set,
# which only the hidden debug menu's toggles touch): the flag poked in the Movie Lot, one primitive of each
# kind added by guest call a frame for four frames (zeroed vectors), under mash (test only).
_DEBUGDRAW = []
for _v in range(3600, 3604):
    _DEBUGDRAW += ['0x1ACA68@%d:0x14B870:#64:0xff' % _v, '0x1ACA68@%d:0x14B8E8:#64:0xff' % _v,
                   '0x1ACA68@%d:0x14B9B0:#16:#16:#16:0xff' % _v, '0x1ACA68@%d:0x14BA58:#16:#16:0xff' % _v,
                   '0x1ACA68@%d:0x14BAE0:#32:#32:0xff:0' % _v]
ROUTES['cov-debugdraw'] = ('route', 'boot; career 1; poke 0x299B24 1; tweaks; mash 1500 147', None,
                           {'ZX_GUEST_CALL': ','.join(_DEBUGDRAW)},
                           'paths7: the engine\'s debug geometry on (a poke) and one primitive of each kind added (test '
                           'only: guest calls), the Movie Lot under mash')
# The input recorder's start saves the players' game objects (each trick object's SpaceNeededForSaving and
# SaveToBuffer) and its playback restores them (LoadFromBuffer): paths5's cov-replay2 started recordings at
# fixed vsyncs, so what was saved was whatever ran then (never a cess slide; grinds saved, none restored).
# Here each recording starts from a trick object's own per-frame update (the guest call's HOOK: a grind's
# 0x1E8B88, a manual's 0x1E7EC0, a cess slide's 0x1EC970, a stall's 0x1EA5E8: the call made while that trick
# runs), then as cov-replay2: mode 3 50 vsyncs on, the playback 10 later, off 140 after that (`+N`: each
# vsync counted from the call before), under mash in the Movie Lot's free skate (test only).
_REPLAY3 = []
for _hook in (0x1E8B88, 0x1E7EC0, 0x1EC970, 0x1EA5E8) * 2:
    _REPLAY3 += ['0x%X@+%d:0x1D9908:1' % (_hook, 200 if _REPLAY3 else 3300), '0x1D9CF8@+50:0x1D9908:3',
                 '0x1D9CF8@+10:0x1D9908:2', '0x1D9CF8@+140:0x1D9908:0']
ROUTES['cov-replay3'] = ('route', 'boot; freeskate 0; tweaks; mash 14000 99', None,
                         dict(_UNLOCKED, ZX_GUEST_CALL=','.join(_REPLAY3)),
                         'paths7: free skate, the input recorder started while a grind, a manual, a cess slide or a '
                         'stall runs and played back (test only)')
# The same recorder start, from the update of a skitch (0x1ED8F8), a pole (0x1ED170), a vault (0x1EC040) and a
# wall trick (0x1EB7A8) in the Tutorial's lessons: the objects of those trick classes' SpaceNeededForSaving, Save
# (the record start) and Load (the playback; test only: guest calls).
_REPLAY4 = []
for _hook in (0x1ED8F8, 0x1ED170, 0x1EC040, 0x1EB7A8):
    _REPLAY4 += ['0x%X@%s:0x1D9908:1' % (_hook, '+200' if _REPLAY4 else '300'), '0x1D9CF8@+50:0x1D9908:3',
                 '0x1D9CF8@+10:0x1D9908:2', '0x1D9CF8@+140:0x1D9908:0']
ROUTES['cov-replay4'] = ('route', 'boot; tutorial; skitch; skitchgo; hpole; vpolea; vault; wallride; walljump', None,
                         dict(FRONT_END_SKIPPED, ZX_GUEST_CALL=','.join(_REPLAY4)),
                         'paths8: the Tutorial\'s skitch, pole, vault and wall ride lessons with the input recorder '
                         'started and played back while each trick runs (test only)')
# Messages the game sends only from places no route reaches, sent by guest call (test only) from a trick handler's
# hook, `%*@4` being the handler's note (object) and `%**(@4+4)` its argument's first word (the player): getScore and
# getMultiplierBonus to a ground object (none answers: groundObjectGetScore), sendTrickStartHolding and
# sendTrickStoppedHolding (what a skater animation's event 2 and 3 do: the manual trick's handlers; none of the
# skater's animation events is one that a manual's runs through), ButtonReleased to an air trick (the game sends it
# only to a pole), and a cess slide's and an air combo's trick ids (a variant; a grab id and a flip id).
_MSGS = ['0x1E6EE0@300:~0x1B1930:%*@4:%*0x2762F4:0:#16', '0x1E6EE0@+2:~0x1B1930:%*@4:%*0x2762F8:0:#16',
         '0x1E7EC0@300:~0x1B36D8:%**(@4+4):0:F0.25', '0x1E7EC0@+3:~0x1B3738:%**(@4+4):0:F0.5',
         '0x1E7EC0@+3:~0x1B36D8:%**(@4+4):0:F0.75', '0x1E7EC0@+3:~0x1B3738:%**(@4+4):0:F0.0',
         '0x1EDFF8@300:~0x1B1930:%*@4:%*0x276320:0:#16', '0x100968@900:0x1ECD40:2', '0x100968@901:0x1EEDC0:0x10:0x100']
ROUTES['cov-msgs'] = ('route', 'boot; tutorial; moves', None, dict(FRONT_END_SKIPPED, ZX_GUEST_CALL=','.join(_MSGS)),
                      'paths8: the Tutorial\'s moves with messages sent by guest call (test only): ground object scores, '
                      'a manual\'s hold messages, ButtonReleased to an air trick, two trick ids')
# The Manuals lesson (the manual's update 0x1E7EC0 runs only once the skater is in a manual: a closed-loop move that
# a slower run misses now and then, so three tries) with the hold messages alone.
ROUTES['cov-manualhold'] = ('route', 'boot; tutorial; manual; manual; manual', None,
                            dict(FRONT_END_SKIPPED, ZX_GUEST_CALL=','.join(_MSGS[2:6])),
                            'paths8: the Tutorial\'s Manuals lesson with TrickStartHolding and TrickStoppedHolding sent to '
                            'the manual by guest call (test only)')
# The Vertical Poles lesson: on the pole, the grab button let go and tapped (a press of it while on a pole tells the
# pole ButtonReleased: tools/routes.py polebutton).
ROUTES['cov-polebutton'] = ('route', 'boot; tutorial; polebutton 1,2', None, FRONT_END_SKIPPED,
                            'paths8: the Tutorial\'s Vertical Poles lesson, the grab button tapped while on the pole')
# The movies through the game's own ps2Movie code again (paths6's cov-movies stalled in the first logo:
# the native sound driver left the movie's audio ring undrained; with ZX_SOUND=spu2 the IOP's SDRDRV
# drains it, drafter24): the careercheck input to vsync 3000 (recorded with --only the movie drafts: with
# every unrecorded target this boot grew past 8 GB by vsync 129).
ROUTES['cov-movies2'] = ('pad', 'careercheck', 3000, {'ZX_MOVIES': 'hle', 'ZX_SOUND': 'spu2'},
                         'paths7: the careercheck input to vsync 3000 through the game\'s own movie code '
                         '(ZX_MOVIES=hle, ZX_SOUND=spu2: the logos and the intro play)')
# The volume of a movie that is playing: setMovieAudioVolume (0x11A558, no caller in the game) by
# guest call during the logos, which have no audio: g_movieHasAudio (0x2736E8) set around each call,
# so setAudioDecVolume runs (zgehand20; test only).
ROUTES['cov-zgh20-moviesound'] = ('pad', 'careercheck', 3000,
                                  {'ZX_MOVIES': 'hle', 'ZX_SOUND': 'spu2',
                                   'ZX_GUEST_CALL': ','.join('0x11A188@%d:W:0x2736E8:1,0x11A188@+1:0x11A558:0:%d,0x11A188@+1:W:0x2736E8:0' % (_v, _a)
                                                               for _v, _a in ((300, 100), (500, 255), (700, 0)))},
                                  'zgehand20: the careercheck boot through the game\'s own movie code, the audio '
                                  'volume set by guest call all along (test only)')
# LOCAL, AI's level 9 (a January 2002 test level no menu reaches): the Movie Lot's free skate restarted with the level byte set to 9
# (routes.py restartlevel 9, the port's Restart Level: ZX_GAME_SETTINGS_MENUS=1), so LOCAL loads with its own
# free-skate script LOCALT.BIN, the one shipped script (with TUT/MOVIE.BIN and MP/LOCAL) that calls
# setBonusObjectiveState; its bonus objective 0 set to `wait` by guest call (FUN_001BE308, as the script's
# grindEnd over 15 does; test only) after the second level load (`+N`), skated under mash.
ROUTES['cov-local'] = ('route', 'boot; freeskate 0; states 100; restartlevel 9; states 100; tweaks; mash 5000 132', None,
                       {'ZX_GAME_SETTINGS_MENUS': '1',
                        'ZX_GUEST_CALL': '0x1A53D0@1:0x13F4B0,0x1A53D0@+1:0x13F4B0,0x1ACA68@+300:0x1BE308:0:0x5b5e4,'
                                           '0x1ACA68@+1500:0x1BE308:0:0x5b5e4'},
                       'paths7: LOCAL (level 9, no menu reaches it) in free skate through Restart Level, its bonus '
                       'objective set (test only), skated')
# The level load (FUN_001A53D0, the top-level state 0's enter: paths6's -w routes kept 9 calls, all Career or
# the Park Editor's) in the other modes, one route each (recorded with --new-blocks: kept where it runs new
# blocks, e.g. split screen's per-player loops): split screen and a Timed Run (the Tutorial's load and the
# port's Restart Level, the loader entered from a level, ran no new block: taken out).
for _name, _cmds, _env in (('mp', 'splitscreen; states 100', {'ZX_PAD_PORT1': 'mirror'}),
                           ('timed', 'timedrun 2; states 100', {}),
                           ):
    ROUTES['cov-loads-' + _name] = ('route', 'boot; ' + _cmds, None, _env,
                                   'paths7: a level loaded (%s), the level load recorded where it runs new blocks' % _cmds)


def _guest_script(calls, strings, at=1500, per=8, hook=0x100968, gap=1):
    """ZX_GUEST_CALL and ZX_GUEST_STRINGS for (target, args...) calls run from HOOK (renderFrame) from
    vsync AT, PER calls a vsync; an argument 'sTEXT' is a string (passed as $N), others go as given; a target
    given as the string '~0xADDR' is made as an invocation that may wait (paths8); GAP vsyncs between the groups.
    A call whose first element is '@N' (then the target) runs N vsyncs after the one before."""
    out, text = [], list(strings)
    vsync, group = at, 0
    for _i, _call in enumerate(calls):
        _call = list(_call)
        _step = int(_call.pop(0)[1:]) if isinstance(_call[0], str) and _call[0].startswith('@') else None
        _t, *_a = _call
        if _step is not None:
            vsync, group = vsync + _step, 1
        elif _i == 0:
            group = 1
        elif group >= per:
            vsync, group = vsync + gap, 1
        else:
            group += 1
        _args = []
        for _x in _a:
            if isinstance(_x, str) and _x.startswith('s'):
                text.append(_x[1:])
                _args.append('$%d' % (len(text) - 1))
            else:
                _args.append(_x if isinstance(_x, str) else '%#x' % _x)
        # a string target '~0xADDR' is called as an invocation that may wait (paths8: guest_calls.cpp)
        out.append('0x%X@%d:%s%s' % (hook, vsync, _t if isinstance(_t, str) else '0x%X' % _t,
                                     ''.join(':' + _y for _y in _args)))
    return {'ZX_GUEST_CALL': ','.join(out), 'ZX_GUEST_STRINGS': '|'.join(text)}


# zfile's device API (the wrappers FUN_0013F528 .. FUN_0013F998, ops 0-4 on a device's method table, 5-10 on
# a path, each a device method: the methods past open, read, seek and close no shipped code calls but the
# watcher task and the dead ZAxis saves FUN_001D8550 / FUN_001D8780), by guest call in the front end on dvd0,
# pcfs and rd00 (test only; mc00's methods wait for the card, which a guest call cannot): a zeroed out
# buffer (#16) and no completion each; deviceSecondMethod (FUN_0013F3C0: the set-attributes slot); then a
# ram-disk file (rd00:ADDR:SIZE over commandLoadLib's own buffer 0x2AA6F8, idle in the front end) opened
# for writing, written, sized and closed, opened again, read and closed (openFile 0x13FA00, fileWrite
# 0x13FD10, fileSize 0x13FED8, closeFile 0x13FBF0, fileRead 0x13FC78). No device that writes to the host
# (pcfs and dvd0 writes would reach the CD root folder) is written to.
_DEVICES = (0x23E380, 0x2347B0, 0x240538)
_DEV_PATHS = ('sdvd0:\\ASSETS\\UILIB.ZAL', 'spcfs:ASSETS\\UILIB.ZAL', 'srd00:0x2AA6F8:0x100', 'sdvd0:\\ASSETS',
              'spcfs:ASSETS')
_DEVAPI = []
for _d in _DEVICES:
    _DEVAPI += [(0x13F528, _d, '#16', 0), (0x13F590, _d, 0), (0x13F5E8, _d, '#16', 0), (0x13F650, _d, 's0:0', '#16', 0),
                (0x13F6C8, _d, 's0:0', '#16', 0), (0x13F3C0, _d, 0, 0, 0)]
for _p in _DEV_PATHS:
    _DEVAPI += [(_f, _p, '#16', 0) for _f in (0x13F740, 0x13F7B8, 0x13F830, 0x13F8A8, 0x13F920)] + [(0x13F998, _p, 0)]
_RAMFILE = 'srd00:0x2AA6F8:0x100'
_N = len(_DEVAPI)
_DEVAPI += [(0x13FA00, _RAMFILE, 2, 0), (0x13FD10, '^%d' % _N, 's0123456789ABCDEF', 16, 0),
            (0x13FED8, '^%d' % _N, '#16', 0), (0x13FBF0, '^%d' % _N, 0),
            (0x13FA00, _RAMFILE, 1, 0), (0x13FC78, '^%d' % (_N + 4), '#32', 16, 0), (0x13FED8, '^%d' % (_N + 4), '#16', 0),
            (0x13FBF0, '^%d' % (_N + 4), 0)]
ROUTES['cov-devapi'] = ('route', 'boot; wait 1700; career 1; skate 1', None, _guest_script(_DEVAPI, [], per=6),
                        'paths7: zfile\'s device API on dvd0, pcfs and rd00, a ram-disk file written and read (test '
                        'only: guest calls in the front end), then the Movie Lot')
# Round paths8. The memory card as zfile's device mc00 (FUN_0011A5E0 registers it at 0x23FD00: the Ps2MemCard
# methods, 20 slots: name, set attributes, is formatted, format, is present, mount, free space, list, make and
# remove directory, find, find at, remove file, open, close, read, write, seek, tell, size), which no shipped
# code opens a file on (only the dead ZAxis dev saves use "mc00:"). Its calls wait (sceMc*), so a plain guest
# call cannot make them; ZX_GUEST_CALL's `~` target makes one as an invocation on the thread (guest_calls.cpp,
# paths8). The wrappers FUN_0013F528 .. FUN_0013F998 and openFile/fileWrite/fileRead/fileSize/closeFile/
# fileSeek with the device's own path "mc00:\\..." and no completion, the front end, an empty card in slot 1:
# mount "0:0", is formatted / present / free space, a directory made twice, listed and found, a file absent,
# opened for writing, written, sized, told, seeked, closed, found, opened for reading, read, removed; the
# directory removed; set attributes; then the device methods called directly with a completion block (the
# poll task 0x121590 and the completions: heap blocks, as the call returns before they run), the card
# formatted. ZX_MC_FD_BASE0=1: this runtime numbers libmc's file descriptors from 1, real libmc from 0,
# and the device layer (FUN_00121138) takes only an open that returns 0 (test only).
_MC = 0x23FD00
_MCD, _MCF = 'smc00:\\P8DIR', 'smc00:\\P8DIR\\P8.DAT'
_MCCB, _MCOUT, _MCLST, _MCREC = '^25', '^26', '^27', '^28'  # heap blocks: completion, an out word, a buffer, a file record
_MCCALLS = [
 ('~0x13F650', _MC, 's0:0', '#16', 0),   # 0 mount
 ('~0x13F528', _MC, '#16', 0),           # 1 is formatted
 ('~0x13F5E8', _MC, '#16', 0),           # 2 is present
 ('~0x13F6C8', _MC, 's0:0', '#16', 0),   # 3 free space
 ('~0x13F7B8', _MCD, '#16', 0),            # 4 make directory
 ('~0x13F7B8', _MCD, '#16', 0),            # 5 again (exists)
 ('~0x13F740', 'smc00:\\*', '#64', 0),  # 6 list
 ('~0x13F740', _MCD + '\\*', '#64', 0),    # 7
 ('~0x13F8A8', _MCD, '#16', 0),            # 8 find
 ('~0x13F8A8', _MCF, '#16', 0),            # 9 find (absent)
 ('~0x13FA00', _MCF, 1, 0),                # 10 open for reading: absent
 ('~0x13FA00', _MCF, 2, 0),                # 11 open for writing
 ('~0x13FD10', '^11', 's0123456789ABCDEF', 16, 0),   # 12 write
 ('~0x13FED8', '^11', '#16', 0),        # 13 size
 ('~0x122420', '^11', '#16', 0),        # 14 tell (device method directly)
 ('~0x13FDA8', '^11', 4, 0, 0),         # 15 seek
 ('~0x13FBF0', '^11', 0),               # 16 close
 ('~0x13F920', _MCF, '#16', 0),            # 17 find at
 ('~0x13FA00', _MCF, 1, 0),                # 18 open for reading
 ('~0x13FC78', '^18', '#32', 16, 0),    # 19 read
 ('~0x13FED8', '^18', '#16', 0),        # 20
 ('~0x13FBF0', '^18', 0),               # 21
 ('~0x13F998', _MCF, 0),                   # 22 remove file
 ('~0x13F830', _MCD, '#16', 0),            # 23 remove directory
 ('~0x13F3C0', _MC, 0, 0, 0),            # 24 set attributes
 # the asynchronous forms: the device methods called directly with a completion block start the poll task
 # (0x121590); the blocks are heap blocks (the call returns before the completion runs; a block on the stack
 # would be written into later: a return address)
 ('@60', '~0x138008', 0x100),           # 25 the completion block
 ('~0x138008', 0x100),                   # 26
 ('~0x138008', 0x100),                   # 27
 ('~0x138008', 0x100),                   # 28
 ('0x148638', _MCCB, 's\x40\x17\x12'),      # 29 its function word 0x121740 (the string's NUL is the high byte)
 ('@2', '~0x121C78', _MC, _MCD, _MCOUT, _MCCB),    # make directory
 ('@8', '~0x121E28', _MC, _MCD, _MCOUT, _MCCB),    # find
 ('@8', '~0x121B98', _MC, _MCD + '\\*', _MCLST, _MCCB),   # list
 ('@8', '~0x121AD0', _MC, 's0:0', _MCOUT, _MCCB),   # free space
 ('@8', '~0x1217A0', _MC, _MCOUT, _MCCB),       # is formatted
 ('@8', '~0x121908', _MC, _MCOUT, _MCCB),       # is present
 ('@8', '~0x1219C8', _MC, 's0:0', _MCOUT, _MCCB),   # mount
 ('@8', '~0x122028', _MCREC, 's\\P8DIR\\A.DAT', 2, _MCCB),   # open for writing
 ('@8', '~0x1222F0', _MCREC, 's0123456789ABCDEF', 16, _MCCB),   # write
 ('@8', '~0x122390', _MCREC, 4, 0, _MCCB),   # seek
 ('@8', '~0x122420', _MCREC, _MCOUT, _MCCB),    # tell
 ('@8', '~0x1221A8', _MCREC, _MCCB),         # close
 ('@8', '~0x122028', _MCREC, 's\\P8DIR\\A.DAT', 1, _MCCB),   # open for reading
 ('@8', '~0x122250', _MCREC, _MCLST, 16, _MCCB),   # read
 ('@8', '~0x1221A8', _MCREC, _MCCB),
 ('@8', '~0x121F60', _MC, 's\\P8DIR\\A.DAT', _MCCB),   # remove file
 ('@8', '~0x121D48', _MC, _MCD, _MCOUT, _MCCB),    # remove directory
 ('@8', '~0x121AD0', _MC, 's0:0', _MCOUT, _MCCB),   # two in the same frame: the second finds the first running
 ('@0', '~0x121908', _MC, _MCOUT, _MCCB),
 ('@20', '~0x121860', _MC, _MCCB),           # format
 ('@20', '~0x1217A0', _MC, _MCOUT, 0),
]
ROUTES['cov-mc00'] = ('route', 'boot; wait 3000', None, dict(_guest_script(_MCCALLS, [], per=1), ZX_MC_FD_BASE0='1'),
                      'paths8: the memory card as zfile\'s mc00 device (test only: guest calls as invocations, file '
                      'descriptors numbered from 0), synchronous and with completions, the front end')


# hand-platform2: functions no input reaches, called by guest call (test only). The sound script's end and a
# sound set's note on records built in commandLoadLib's idle buffer (as cov-devapi uses it); the streams' close
# and the no-caller stream sample in the front end; lines added to the debug list once the debug draw is on.
_HP2_AT = 0x2AA6F8
_HP2_WORDS = [0] * 64
_HP2_WORDS[0] = 0x0800                                  # source A: repeats
_HP2_WORDS[0x24 // 4] = _HP2_WORDS[0x28 // 4] = _HP2_AT + 0x50  # its script, at +0x50
_HP2_WORDS[0x3C // 4] = 0x1665C8                        # a callback that does nothing (saveApiNoop)
_HP2_WORDS[0x50 // 4] = 0x00002003                      # coding 3: one length byte
_HP2_WORDS[0x60 // 4:0x70 // 4] = [_HP2_AT, 0, 0x0201, 0]        # its end: first 1, second 2
_HP2_WORDS[0xC0 // 4:0xD0 // 4] = [_HP2_AT + 0x70, 0, 0x0001, 0] # source B (zeros): stops
_HP2_WORDS[0xD0 // 4:0xE0 // 4] = [_HP2_AT + 0x70, 0, 0x0300, 0] # first 0: only the callback's test
_HP2_SET = '=0x00000210/0x00FF0100'                     # first note 0x10, two notes: a sample, none
_HP2_CALLS = [(0x136B60, _HP2_AT, '=' + '/'.join('%#x' % _w for _w in _HP2_WORDS), 0x100)]
_HP2_CALLS += [(0x16EBA0, _HP2_AT + _o) for _o in (0x60, 0xC0, 0xD0)]
_HP2_CALLS += [(0x16ED38, _HP2_SET, _n, '#16', '#16') for _n in (5, 0x10, 0x11, 0x12)]
_HP2_SAMPLE = len(_HP2_CALLS)
_HP2_CALLS += [('@4', '~0x113AF0'), ('@4', 0x1137D8, 0, 0), ('@4', '~0x1140B0', '^%d' % _HP2_SAMPLE + '', 0, 0x40, 0x7F), ('@30', '~0x113AF0')]
ROUTES['cov-hp2-calls'] = ('route', 'boot; wait 2400', None, _guest_script(_HP2_CALLS, []),
                           'hand-platform2: sourceDataEnded, soundSetEntry, closeAllSoundStreams, playSoundStreamSample by guest call')
_HP2_LINE = '=0x41200000/0x41200000/0x41200000/0x3F800000'
_HP2_DEBUG = ['0x1ACA68@%d:0x14B920:%s:%s:=0xFF00FF00:0xff' % (_v, _HP2_LINE, _HP2_LINE) for _v in range(3600, 3604)]
ROUTES['cov-hp2-debuglines'] = ('route', 'boot; career 1; poke 0x299B24 1; tweaks; mash 1500 147', None, {'ZX_GUEST_CALL': ','.join(_HP2_DEBUG)},
                                'hand-platform2: lines on the debug list with the debug draw on: drawDebugLines')

# zgui's variable types' value setters (the type tables' setFloat, setInt, setVector slots: +0x4C, +0x50, +0x5C; only
# operateFloats calls one, with a ZFloat destination, so no script or native reaches the Uint8, Bool, Sint32, palette
# and vector types', nor the ZFloat's setInt): a variable of each type declared (the command dispatcher 0x1820F8), its
# node found (findNode 0x17F918) and the setters called on it with a value (a float, an int, a colour) by guest call
# in the front end (test only).
_ZSET_TYPES = (('Uint8', 'U8'), ('Bool', 'B'), ('Sint32', 'S'), ('ZFloat', 'F'), ('ZRGBA_PAL', 'P'), ('Z3DVector', 'V'))
_ZSET = [(0x1820F8, 'sDECLARE %s Root.P8%s' % _t) for _t in _ZSET_TYPES]
_ZSET += [(0x17F918, '%*0x240A1C', 'sRoot.P8' + _t[1], 2) for _t in _ZSET_TYPES]
_ZN = {_t[1]: '^%d' % (6 + _i) for _i, _t in enumerate(_ZSET_TYPES)}
for _v in ('=0x3FC00000', '=0x42C80000', '=0xBF800000', '=0x437F0000', '=0x0'):    # 1.5, 100, -1, 255, 0
    _ZSET += [('~0x1805C0', _ZN['U8'], _v), ('~0x180720', _ZN['B'], _v), ('~0x1808F8', _ZN['S'], _v),
              ('~0x180D78', _ZN['P'], _v), ('~0x180FF0', _ZN['P'], _v)]
for _v in ('=7', '=300', '=0xFFFFFFFF', '=0x0'):
    _ZSET += [('~0x180A70', _ZN['F'], _v), ('~0x180E10', _ZN['P'], _v)]
for _v in ('=0x0A0B0C0D', '=0xFFFFFFFF', '=0x0'):
    _ZSET += [('~0x181338', _ZN['V'], _v)]
_ZSET += [(0x1820F8, 'sUNDECLARE Root.P8' + _t[1]) for _t in _ZSET_TYPES]
ROUTES['cov-zguiset'] = ('route', 'boot; wait 2400', None, _guest_script(_ZSET, [], at=1700, per=6),
                         'paths8: zgui variable types\' value setters called on declared variables (test only: guest '
                         'calls in the front end)')
# Pure library and math functions no game input reaches, called by guest call in the front end (test only; the
# arguments are the functions' own kinds: floats, ints, strings, vectors, matrices, quaternions in blocks `=w/w/..`):
# libm's ceilf, floorf, asinf and its core, acosf's domain error (matherr, __errno), scalbnf, copysignf, sinf, cosf
# and tanf past |x| > 2**7 pi/2 (__kernel_rem_pio2f), the engine's string span functions, vec3RotateZ and
# vec3RotateEuler, matrixTransformPoints, quaternionToAxisAngle, quaternionNlerp, quaternionAlign.
import struct as _struct
def _fs(*xs): return '=' + '/'.join('%#x' % _struct.unpack('<I', _struct.pack('<f', x))[0] for x in xs)
def _libcalls():
    _LIBCALLS = []
    for x in (2.5, -2.5, 0.0, -0.3, 3.0, 1e10, -1e10, 1e-30, 12345.678, 0.99999, -1.0, 8388608.5, 1.5e38):
        _LIBCALLS += [('~0x202488', 'F%r' % x), ('~0x202668', 'F%r' % x)]                   # ceilf, floorf
    for x in (0.5, -0.5, 1.0, -1.0, 2.0, 0.0, 1e-10, 0.9, 0.99999, 1.0000001):
        _LIBCALLS += [('~0x2029C8', 'F%r' % x), ('~0x203020', 'F%r' % x)]                   # asinf, __ieee754_asinf
    for x in (2.0, -2.0, 1.5):
        _LIBCALLS += [('~0x2028C8', 'F%r' % x)]                                              # acosf: domain error: matherr, __errno
    _LIBCALLS += [('~0x202AC8', 'F0.0', 'F0.0'), ('~0x202AC8', 'F1.0', 'F0.0')]            # atan2f
    for x, n in ((1.0, 5), (1.0, -5), (3.0, 200), (3.0, -200), (1.0, 128), (1.0, -149), (0.0, 3), (1e-40, 3), (1e-40, -3),
                 (1.5, 50000), (1.5, -50000), (float('inf'), 2), (float('nan'), 2), (-2.0, 3)):
        _LIBCALLS += [('~0x204A58', 'F%r' % x, n)]                                            # scalbnf(x, n)
    for x, y in ((1.0, -2.0), (-1.0, 2.0), (3.0, 0.0), (0.0, -1.0), (-4.5, -4.5)):
        _LIBCALLS += [('~0x204BB8', 'F%r' % x, 'F%r' % y)]                                    # copysignf
    for x in (1000.0, -1000.0, 1e6, 3e9, 1e20):
        _LIBCALLS += [('~0x202750', 'F%r' % x), ('~0x202560', 'F%r' % x), ('~0x202840', 'F%r' % x)]   # sinf, cosf, tanf: |x| > 2**7 pi/2
    _LIBCALLS += [('~0x148848', 'sHello World', 'shello WORLD', 11), ('~0x148848', 'sabc', 'sabd', 3), ('~0x148848', 'sabc', 'sabC', 2),
              ('~0x148848', 'sabc', 'sab', 0)]
    _LIBCALLS += [('~0x1488F8', 'sHello World', 'sol'), ('~0x1488F8', 'sHello', 'sxyz'), ('~0x1488F8', 'sHello', 's')]
    _LIBCALLS += [('~0x148990', 'saabbcc', 'sab'), ('~0x148990', 'sxyz', 'sab'), ('~0x148990', 'saaa', 'sa')]
    _LIBCALLS += [('~0x148A38', 'sHello World', 'so'), ('~0x148A38', 'sabc', 'sxyz'), ('~0x148A38', 'sabc', 's')]
    for a in (0, 0x2000, 0x4000, -0x2000, 0x7FFF):
        _LIBCALLS += [('~0x137040', '#16', _fs(1.0, 2.0, 3.0), a & 0xFFFFFFFF)]               # vec3RotateZ(dst, src, angle)
    _LIBCALLS += [('~0x1370C8', '#16', _fs(1.0, 2.0, 3.0), '=0x2000/0x4000/0x6000'), ('~0x1370C8', '#16', _fs(0.5, -2.0, 1.0), '=0xFFFFE000/0/0x1000')]
    ID = _fs(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
    M = _fs(0.5, 0.25, 0, 0, -0.25, 0.5, 0, 0, 0, 0, 1, 0, 10.0, -20.0, 3.0, 1)
    P = _fs(1, 2, 3, 1, -4, 5, -6, 1, 0, 0, 0, 1)
    _LIBCALLS += [('~0x137120', '#64', M, P, 3), ('~0x137120', '#64', ID, P, 1), ('~0x137120', '#64', M, P, 0)]    # matrixTransformPoints
    Q = _fs(0.0, 0.0, 0.0, 1.0)
    Q2 = _fs(0.5, 0.5, 0.5, 0.5)
    Q3 = _fs(-0.5, -0.5, -0.5, -0.5)
    Q4 = _fs(0.0, 0.7071068, 0.0, 0.7071068)
    _LIBCALLS += [('~0x14CC18', '#16', Q), ('~0x14CC18', '#16', Q2), ('~0x14CC18', '#16', Q4)]            # quaternionToAxisAngle(axis, q)
    _LIBCALLS += [('~0x14D610', '#16', Q, Q2, 'F0.5'), ('~0x14D610', '#16', Q2, Q3, 'F0.25'), ('~0x14D610', '#16', Q, Q4, 'F1.0')]   # nlerp
    _LIBCALLS += [('~0x14D868', '#16', Q2, Q), ('~0x14D868', '#16', Q3, Q2), ('~0x14D868', '#16', Q4, Q4)]   # quaternionAlign
    return _LIBCALLS


ROUTES['cov-libcalls'] = ('route', 'boot; wait 3400', None, _guest_script(_libcalls(), [], at=1700, per=1),
                          'paths8: libm, string span, vector, matrix and quaternion functions no caller reached, by guest '
                          'call in the front end (test only)')

# The game's CD client again (ZX_DISC=ps2), through zfile's dvd0 device by guest call in the front end (test only):
# the device API as invocations (find, list, free space, an open, size, seek, read and close of UILIB.ZAL, a missing
# file), then the open and the size with a completion block (heap blocks: the completion's function word is made
# by strcpy): the check functions (0x11B4D8 ..) are the CD client's callbacks, passed only with a completion. A
# third call with a completion waits for the CD client's reply on the thread that has to run it (the main one):
# the run ends there.
_CDDV = 0x23E380
_CDF = 'sdvd0:\\ASSETS\\UILIB.ZAL'
_CDCALLS = [
    ('~0x13F528', _CDDV, '#16', 0), ('~0x13F5E8', _CDDV, '#16', 0), ('~0x13F650', _CDDV, 's0:0', '#16', 0),
    ('~0x13F6C8', _CDDV, 's0:0', '#16', 0), ('~0x13F740', 'sdvd0:\\ASSETS', '#64', 0), ('~0x13F8A8', _CDF, '#16', 0),
    ('~0x13F8A8', 'sdvd0:\\NOSUCH.DAT', '#16', 0), ('~0x13F920', _CDF, '#16', 0), ('~0x13FA00', _CDF, 1, 0),
    ('~0x13FED8', '^8', '#16', 0), ('~0x13FDA8', '^8', 64, 0, 0), ('~0x13FC78', '^8', '#64', 32, 0),
    ('~0x13FED8', '^8', '#16', 0), ('~0x13FBF0', '^8', 0), ('~0x13FA00', 'sdvd0:\\NOSUCH.DAT', 1, 0),
    ('~0x138008', 0x100), ('~0x138008', 0x100), ('~0x138008', 0x400), ('~0x138008', 0x100),
    ('0x148638', '^15', 's\x40\x17\x12'),
    ('@20', '~0x11BFB0', '^18', 's\\ASSETS\\UILIB.ZAL', 1, '^15'), ('@20', '~0x11CC00', '^18', '^16', '^15')]
ROUTES['cov-cdvd2'] = ('route', 'boot; wait 3200', None, dict(_guest_script(_CDCALLS, [], at=1700, per=1), ZX_DISC='ps2'),
                       'paths8: the game\'s CD client driven through zfile\'s dvd0 device (ZX_DISC=ps2; test only: guest '
                       'calls in the front end)')
def _p8_lib2():
    # asset libraries opened by the real API (openAssetLibrary 0x140968 on FX.ZAL and UILIB.ZAL as zfile paths), then the
    # selectors, the readers and the walker no caller used (assetCount .. nextAsset, findAsset, readAsset, forEachAsset),
    # the library closed
    def lib(path, base):
        n = base
        c = [('~0x140968', path)]                                             # n: the record
        L = '^%d' % n
        c += [('~0x140D90', L), ('~0x140D98', L), ('~0x140DA0', L), ('~0x140DB8', L)]       # count, current, id (n+3), name
        c += [('~0x140E30', L, 2), ('~0x140E30', L, 100000), ('~0x140DA0', L), ('~0x140DB8', L), ('~0x140E78', L), ('~0x140EA0', L)]
        c += [('~0x140E50', L), ('~0x140E50', L), ('~0x140DA0', L)]
        c += [('~0x140AC8', L, '^%d' % (n + 3)), ('~0x140AC8', L, 0x7FFFFFFF)]               # findAsset by the first id; absent
        c += [('~0x138008', 0x80000)]                                                      # a buffer (n+15)
        c += [('~0x140E30', L, 1), ('~0x140B18', L, '^%d' % (n + 15), 0)]                # readAsset of entry 1
        c += [('~0x140CC8', L, 0x121740, 0)]                                              # forEachAsset (function: a benign one)
        c += [('~0x140A28', L)]                                                           # close
        return c
    CALLS = lib('sASSETS\\FX.ZAL', 0)
    CALLS += lib('sASSETS\\UILIB.ZAL', len(CALLS))
    CALLS += [('~0x140EE0',), ('~0x140B10',), ('~0x140868',)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_lib2():
    g = _p8_lib2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-assetlib'] = ('route', 'boot; wait 2500', None, dict(_p8_env_lib2()),
              "paths8: asset libraries (FX.ZAL, UILIB.ZAL) opened through the engine's own openAssetLibrary and walked by guest call (test only): selectors, readAsset, forEachAsset, close")

def _p8_files1():
    # zfile's small API no caller used (takeFileError, fileRequestsInFlight, cancelFileRequests, fileDeviceName,
    # loadFileBytes, pathBaseName) by guest call in the front end (test only): on a real open file, real paths
    F = 'sASSETS\\UILIB.ZAL'
    CALLS = [
     ('~0x13F4D0',), ('~0x13EBD0',),
     ('~0x13FA00', F, 1, 0),                                # 2: the file
     ('~0x13EB10', '^2'), ('~0x13FF48', '^2'),
     ('~0x13FBF0', '^2', 0),
     ('~0x140080', F, 64), ('~0x140080', 'sSYSTEM.CNF', 0x1000), ('~0x140080', 'sNOSUCH.DAT', 64), ('~0x140080', F, 0x40000),
     ('~0x1403A8', F, '#64'), ('~0x1403A8', 'sASSETS\\LEVELS\\CIVIC\\CIVIC.GEO', '#64'), ('~0x1403A8', 'snodot', '#64'),
     ('~0x1403A8', 'sa.b.c', '#64'), ('~0x1403A8', 's\\\\', '#64'), ('~0x1403A8', 0, '#64'),
    ]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_files1():
    g = _p8_files1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-filesapi'] = ('route', 'boot; wait 2200', None, dict(_p8_env_files1()),
              "paths8: zfile's small API no caller used (error, in-flight count, cancel, device name, loadFileBytes, pathBaseName) by guest call in the front end (test only)")

def _p8_chunk1():
    # the chunk reader's hooks and trace (setChunkProgressHook / chunkProgressHook, setChunkTrace and the traced
    # chunk entry traceChunk, setAfterChunkHook) by guest call in the front end (test only): a benign hook (0x112EA8:
    # returns 0), tracing on while UILIB.ZAL's ZDIR form is entered (openChunkFile 0x148C30) and an asset library opened
    F = 'sASSETS\\UILIB.ZAL'
    CALLS = [
     ('~0x148AE0', 0x112EA8), ('~0x148AF0',), ('~0x148C18', 1),
     ('~0x13FA00', F, 1, 0),                                  # 3: the file
     ('~0x148C30', '^3', 0x5249445A, 0),                      # 4: its reader ('ZDIR')
     ('~0x148F00', '^4', 0x112EA8), ('~0x148F00', '^4', 0),
     ('~0x148D58', '^4'), ('~0x13FBF0', '^3', 0),
     ('~0x140968', F),                                        # 9: an asset library, its form entered traced
     ('~0x140A28', '^9'),
     ('~0x148C18', 0), ('~0x148AE0', 0), ('~0x148AF0',),
    ]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_chunk1():
    g = _p8_chunk1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-chunkapi'] = ('route', 'boot; wait 2200', None, dict(_p8_env_chunk1()),
              "paths8: the chunk reader's hooks and trace by guest call in the front end (test only)")

def _p8_scene1():
    # scene objects made through the engine's own API (newSceneObject 0x13BA50, initSceneObject 0x13BA78 as kind 3: an
    # object without a class or a scene) and the attachment functions no route used: attached, attached at a place, walked,
    # placed by the parent's matrix, detached, released, deleted (test only: guest calls in the front end; the stale
    # objects' frame task (updateStaleObjects) places the attached ones meanwhile)
    ID = '=0x3f800000/0/0/0/0/0x3f800000/0/0/0/0/0x3f800000/0/0/0/0/0x3f800000'
    CALLS = [('~0x13BA50',)] * 5                                                  # 0-4: A B C D E
    CALLS += [('~0x13BA78', '^%d' % i, 3) for i in range(5)]                      # 5-9
    A, B, C, D, E = ('^%d' % i for i in range(5))
    CALLS += [('~0x13BBF8', A, 0x3F800000), ('~0x13BD98', A, 1), ('~0x13BD98', A, 0), ('~0x13BB58', A)]
    CALLS += [('~0x13BFB0', A), ('~0x13C000', A), ('~0x13C000', A)]               # an attachment made, given back, none to give back
    CALLS += [('~0x13C038', B, A, 0), ('~0x13C038', B, A, 0), ('~0x13C0F8', C, A, 0, 0), ('~0x13C0F8', D, A, 0, 0xFFFFFFFF),
              ('~0x13C0F8', E, A, 0, B), ('~0x13C0F8', E, A, 0, 0)]                # attached, again, first, last, after B, again
    CALLS += [('~0x13C458', A), ('~0x13C438', B), ('~0x13C478', B), ('~0x13C478', A), ('~0x13C458', C), ('~0x13C438', E)]
    CALLS += [('~0x13C4D8', A, 0x121740, 0), ('~0x13BE60', 0x121740, 0, 3)]
    CALLS += [('~0x13C540', B, ID)]                                                # placed by a matrix (no bone)
    CALLS += [('@6', '~0x13C288', A), ('~0x13C288', B), ('~0x13C288', C)]          # A's children let go, the rest
    CALLS += [('~0x13BAE8', o) for o in (A, B, C, D, E)]                           # deleted
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_scene1():
    g = _p8_scene1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-sceneattach'] = ('route', 'boot; wait 2300', None, dict(_p8_env_scene1()),
              "paths8: scene objects made by the engine's API and attached, walked, placed, detached, deleted by guest call in the front end (test only)")

def _p8_anim1():
    # the engine's animator copies and the animation events' queries no caller used, on the skater's own animator and
    # events (test only: guest calls in the Movie Lot's free skate; the player 0x973dd0 = *(*0x24C9BC+0x10)+0x10, its
    # scene node at +0x14): cloneAnimator, copyAnimatorPose, boneScale, copyAnimator, then at the skater's animation
    # event handler (0x194870: a0 the player, a1 the event) eventTime, eventFloat, the two field-address functions, the
    # next and previous events of a kind (kind 0x068B10D7, the one the handler tests; field 0x0619FB37), and last
    # freeAnimator on the clone
    NODE = '%*(*(*0x24C9BC+0x10)+0x24)'
    EV = '%@5'
    G = ['0x100968@4000:~0x144798:' + NODE,                         # 0: the skater's animator
         '0x100968@4001:~0x1451F8:' + NODE,                         # 1: a clone
         '0x100968@4002:~0x144D90:^0:^1', '0x100968@4003:~0x144D90:^1:^0', '0x100968@4004:~0x1448A8:^0',
         '0x100968@4005:~0x145330:^1:' + NODE]
    for k, (fn, extra) in enumerate((('0x144EE8', ''), ('0x144F60', ':0x0619FB37'), ('0x144F60', ':0x12345'), ('0x145008', ':0x0619FB37'),
                                     ('0x145008', ':0x12345'), ('0x145040', ':0x0619FB37'), ('0x145040', ':0x12345'))):
        G.append('0x194870@%s:~%s:%s%s' % ('4010' if k == 0 else '+1', fn, EV, extra))
    for fn in ('0x144E98', '0x144EC0'):
        for kind in ('0x068B10D7', '0x12345678'):
            G.append('0x194870@+1:~%s:%s:%s' % (fn, kind, EV))
    G.append('0x100968@4300:~0x1454E8:^1')
    ENV = {'ZX_GUEST_CALL': ','.join(G)}
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_anim1():
    g = _p8_anim1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-animq'] = ('route', 'boot; freeskate 0; skate 2', None, dict(_p8_env_anim1()),
              "paths8: animator copies and animation event queries on the skater's own animator and events, by guest call in the Movie Lot's free skate (test only)")

def _p8_anim2():
    # animation records made and given back through the engine's own makers (animation_file.cpp's new*/free* pairs), a
    # motion's id, and the animator calls no caller used (advanceAnimationTime, animatorBoneOffset, addPoseLayerNamed
    # with a name that names no animation, shiftTracks and resetAnimatorPose on the skater's node by zero seconds, a scale
    # track's key), by guest call in the Movie Lot's free skate (test only)
    NODE = '%*(*(*0x24C9BC+0x10)+0x24)'
    CALLS = []
    for mk, fr in ((0x140F98, 0x140F88), (0x141000, 0x140FF0), (0x141068, 0x141058), (0x1410D0, 0x1410C0), (0x141138, 0x141128),
                   (0x1411A0, 0x141190)):
        CALLS += [('~0x%X' % mk,), ('~0x%X' % fr, '^%d' % len(CALLS))]
    CALLS += [('~0x138008', 0x100)]
    CALLS += [('~0x144790', '^%d' % (len(CALLS) - 1))]                              # motionId
    CALLS += [('~0x141138',)]                                                       # a scale track (cleared)
    ST = '^%d' % (len(CALLS) - 1)
    CALLS += [('~0x1414F8', ST, '#16', 'F0.5'), ('~0x1414F8', ST, '#16', 'F0.0')]   # sampleScaleKey
    CALLS += [('~0x1AE078', NODE, 'F0.0'), ('~0x1AE078', NODE, 'F0.01'), ('~0x1AE078', 0x100, 'F0.0'),
              ('~0x1ADF48', NODE, '#16'),
              ('~0x1AD8D0', NODE, 'sNoSuchAnimation', 0, 0, 0, 'F0.0', 'F1.0'),
              ('~0x142D08', NODE, 'F0.0'), ('~0x1439C0', NODE)]
    AT = 4000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_anim2():
    g = _p8_anim2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-animfile'] = ('route', 'boot; freeskate 0; skate 2', None, dict(_p8_env_anim2()),
              "paths8: animation records made and freed by the engine's makers, animator calls no caller used, by guest call in the Movie Lot's free skate (test only)")

def _p8_scenedb1():
    # the scene database's small API (SCENE.TXT's entries): an entry by index (lookUpSceneEntry kind 0), its next and
    # first child, an entry made and given back through the pool's callback, a camera of the second kind, a light
    # (the maker makes none in this build) and its release, by guest call in the front end (test only)
    CALLS = [('~0x13D4A8', 0, 0, 0), ('~0x13D4A8', 1, 0, 0), ('~0x13D4A8', 2, 0, 0), ('~0x13D4A8', 9999, 0, 0),
             ('~0x13C770', '^0'), ('~0x13C778', '^0'), ('~0x13C770', '^1'), ('~0x13C778', '^1'),
             ('~0x13C6B8',), ('~0x13C710', '^8'),
             ('~0x13DBA8', 1, 0), ('~0x13DC10', '^10'),
             ('~0x13BA50',), ('~0x13BA78', '^12', 3), ('~0x13DCB8', '^12'), ('~0x13DC50', 1, 0, 0)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_scenedb1():
    g = _p8_scenedb1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


# Round draft1. The Wall Rides lesson's ride with the game's Low Gravity Wall Ride cheat (Wall Ride Gravity 3, a real
# cheat code: docs/CHEATS.md), held on the wall until its trick's animation ends while the trick is still the skater's
# current object (endWallTrickAnimation, setupWallTricks' AnimationEnded handler), then the jump off.
ROUTES['cov-wallend'] = ('route', 'boot; tutorial; tweaks lowgravitywall; wallride; wallride; walljump; wallride', None,
                         FRONT_END_SKIPPED, 'draft1: the Wall Rides lesson with the Low Gravity Wall Ride cheat, three '
                         'rides and a jump off')
ROUTES['cov-scenedb'] = ('route', 'boot; wait 2300', None, dict(_p8_env_scenedb1()),
              "paths8: the scene database's small API (entries by index, next, first child, an entry made and freed, a camera, a light's release) by guest call in the front end (test only)")

def _p8_model1():
    # model object hooks no caller reached, on the skater's own scene object (the node: Player +0x14 of the player
    # 0x973dd0): setModelScale (the class's scaled hook: object, a scale vector) with a non-unit scale then unit again (the
    # pose then goes through matrixScaling, which poseScene calls for a scaled one), modelDebugReport, modelUnusedHook,
    # modelHasAnimator, hideModelObject (the skater hidden at the end), by guest call in the Movie Lot's free skate (test only)
    NODE = '%*(*(*0x24C9BC+0x10)+0x24)'
    CALLS = [('~0x15D7F8', NODE, '=0x3f8ccccd/0x3f8ccccd/0x3f8ccccd/0x3f800000'),
             ('@30', '~0x15D7F8', NODE, '=0x3f800000/0x3f800000/0x3f800000/0x3f800000'),
             ('~0x15D888', NODE), ('~0x15D880',), ('~0x15DD68', NODE), ('~0x15DD68', 0),
             ('~0x137380', '#64', '=0x3f800000/0x40000000/0x3f000000/0'),
             ('@30', '~0x15D938', NODE)]
    AT = 4000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_model1():
    g = _p8_model1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-modelhooks'] = ('route', 'boot; freeskate 0; skate 2', None, dict(_p8_env_model1()),
              "paths8: model object hooks (scale, debug report, animator test, hide) on the skater's own scene object by guest call in the Movie Lot's free skate (test only)")

def _p8_dbg1():
    # the engine's debug text of a vector, three angles and a matrix (x, y, label, values; the label 0 for the other form),
    # by guest call in the front end (test only)
    V = '=0x3f800000/0x40000000/0xc0400000'
    A = '=0x20001000/0x00003000'
    M = '=0x3f800000/0/0/0/0/0x3f800000/0/0/0/0/0x3f800000/0/0x41200000/0xc1a00000/0x40400000/0x3f800000'
    CALLS = [('~0x14BE70', 10, 10, 'sPos', V), ('~0x14BE70', 10, 20, 0, V),
             ('~0x14BF78', 10, 30, 'sAng', A), ('~0x14BF78', 10, 40, 0, A),
             ('~0x14C108', 10, 50, 'sMat', M), ('~0x14C108', 10, 80, 0, M)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_dbg1():
    g = _p8_dbg1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-dbgprint'] = ('route', 'boot; wait 2300', None, dict(_p8_env_dbg1()),
              "paths8: the engine's debug text of a vector, angles and a matrix by guest call in the front end (test only)")

def _p8_zc1():
    # zcompres: decompressFileData (0x13EA50: method, source, its bytes, destination, state) called by guest call in the front end
    # (test only) on token streams made by zc/lzo.py (LZO1X: the format of method 2, decodeFileBlock: literal runs, matches of the
    # 2-, 3- and 4-byte forms, trailing literals, a long overlapping copy, an end marker; an empty one, an unknown method) and
    # method 1's stored form; the libraries' compressed entries (CLEN) are the only game data that uses them and no release
    # ships one (a stream cut off before its end marker runs on past the buffer: none is given)
    CALLS = [('~0x138008', 65536),
             ('~0x13EA50', 2, '=0x43424135/0x47464544/0x4b4a4948/0x4f4e4d4c/0x53525150/0x57565554/0x305a5958/0x34333231/0x38373635/0x78018639/0x75072079/0x487100/0x31300200/0x35343332/0x39383736/0x64636261/0x68676665/0x7206a69/0x480074/0x11', 79, '^0', '#16'),
             ('~0x13EA50', 2, '=0x63626119/0x67666564/0x2068/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x0/0x1cbb0000/0x40291000/0x24001000/0xc423d4c0/0x1100', 184, '^0', '#16'),
             ('~0x13EA50', 2, '=0x700b600/0x231c150e/0x3f38312a/0x5b544d46/0x77706962/0x938c857e/0xafa8a19a/0xcbc4bdb6/0xe7e0d9d2/0x3fcf5ee/0x1f18110a/0x3b342d26/0x57504942/0x736c655e/0x8f88817a/0xaba49d96/0xc7c0b9b2/0xe3dcd5ce/0xfff8f1ea/0x1b140d06/0x37302922/0x534c453e/0x6f68615a/0x8b847d76/0xa7a09992/0xc3bcb5ae/0xdfd8d1ca/0xfbf4ede6/0x17100902/0x332c251e/0x4f48413a/0x6b645d56/0x87807972/0xa39c958e/0xbfb8b1aa/0xdbd4cdc6/0xf7f0e9e2/0x130c05fe/0x2f28211a/0x4b443d36/0x67605952/0x837c756e/0x9f98918a/0xbbb4ada6/0xd7d0c9c2/0xf3ece5de/0xf0801fa/0x2b241d16/0x47403932/0x635c554e/0x18fc716a/0x11', 207, '^0', '#16'),
             ('~0x13EA50', 2, '=0x63626119/0x67666564/0x2068/0x0/0x0/0x1ca200/0x79787701/0x700c0c7a/0x1100', 36, '^0', '#16'),
             ('~0x13EA50', 2, '=0x63626114/0xa7800a9/0x4c7a7a00/0x1100', 16, '^0', '#16'),
             ('~0x13EA50', 2, '=0x67626113/0x33323100/0x110044/0x0', 13, '^0', '#16'),
             ('~0x13EA50', 2, '=0x0', 0, '^0', '#16'),
             ('~0x13EA50', 3, '=0x0', 1, '^0', '#16'),
             ('~0x13EA50', 1, '=0x1/0x33323130/0x37363534/0x42413938/0x46454443', 20, '^0', '#16'),
             ('~0x13EA50', 1, '=0x1', 4, '^0', '#16'),
             ('~0x13EA50', 1, '=0x0/0x42410000/0x46454443/0x4847', 14, '^0', '#16'),
             ('~0x13EA50', 1, '=0x0/0x42410004/0x430003', 11, '^0', '#16'),
             ('~0x13EA50', 1, '=0x0/0x4241000c/0x2250113/0x4443', 16, '^0', '#16')]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_zc1():
    g = _p8_zc1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-zcompres'] = ('route', 'boot; wait 2300', None, dict(_p8_env_zc1()),
              "paths8: zcompres' decompressFileData on token streams made for it (LZO1X for method 2, method 1's stored and literal forms) by guest call in the front end (test only)")

def _p8_timers1():
    # the game's timers' tick and running calls no caller used, on a timer the game's own createTimer makes (no hooks,
    # 100 seconds, no tick), by guest call in the Movie Lot (the timers are set up at a level: createTimer gives 0 before; test only)
    CALLS = [('~0x1DAFC8', 0, 0, 0, 0, 0, 0, 'F100.0', 'F-1.0'),
             ('~0x1DB260', '^0', 'F0.5'), ('~0x1DB260', '^0', 'F-1.0'), ('~0x1DB2E0', '^0', 0), ('~0x1DB318', '^0'),
             ('~0x1DB2E0', '^0', 1), ('~0x1DB318', '^0'), ('~0x1DB070', '^0')]
    AT = 4000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_timers1():
    g = _p8_timers1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-timers'] = ('route', 'boot; freeskate 0; skate 1', None, dict(_p8_env_timers1()),
              "paths8: timers' tick time and running flag by guest call on a timer the game makes (Movie Lot free skate, test only)")

def _p8_api2():
    # pure math, the cycle counters, the texture stubs, the profiler's bars and layout, and the cameras of a kind, by guest
    # call in the front end (test only): angle floor/ceiling/asin (Float arguments), vec3AddScalar, matrixTranslated, libpc's
    # counters, the profiler's application and engine bars made and started, stopped and restarted
    import struct
    def fs(*xs): return '=' + '/'.join('%#x' % struct.unpack('<I', struct.pack('<f', x))[0] for x in xs)
    CALLS = []
    for x in (2.5, -2.5, 0.0, 3.0, 1e10):
        CALLS += [('~0x136C70', 'F%r' % x), ('~0x136C90', 'F%r' % x)]
    for x in (0.5, -0.5, 1.0, 0.0, 2.0):
        CALLS += [('~0x136CF0', 'F%r' % x)]
    CALLS += [('~0x137230', '#16', fs(1.0, 2.0, 3.0), 'F0.5'), ('~0x137230', '#16', fs(-1.0, 0.0, 9.0), 'F-2.0')]
    M = fs(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1)
    CALLS += [('~0x137308', '#64', M, fs(1.0, -2.0, 0.5)), ('~0x137308', '#64', M, fs(0.0, 0.0, 0.0))]
    CALLS += [('~0x119388',), ('~0x119510',), ('~0x119528',), ('~0x119568',), ('~0x1195A0', '#16', '#16'), ('~0x1195A0', 0, '#16'),
              ('~0x1195A0', '#16', 0), ('~0x119608',)]
    CALLS += [('~0x106D20',), ('~0x106D28',), ('~0x106D30',)]
    CAM = len(CALLS)
    CALLS += [('~0x12A180', k) for k in (0, 1, 2, 3)]
    for k in (0, 2):
        c = '^%d' % (CAM + k)
        CALLS += [('~0x129D18', c), ('~0x129DC0', c), ('~0x12A0C8', c, 'F10.0')]
    CALLS += [('~0x12A3C8',)]
    CALLS += [('~0x150950', '=0x20/0/0/8/6/4/4/8/4/1/120/60'), ('~0x150F18', 16, 16), ('~0x150F18', -8, -8)]
    CALLS += [('~0x150B28',)]
    B = '^%d' % (len(CALLS) - 1)
    CALLS += [('~0x150728', B), ('~0x150748', B), ('~0x150768', B), ('~0x150E08', B), ('~0x150E30', B), ('~0x150BF8',)]
    E = '^%d' % (len(CALLS) - 1)
    CALLS += [('~0x150728', E), ('~0x150748', E), ('~0x150E30', E), ('~0x150BF8',)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_api2():
    g = _p8_api2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-api2'] = ('route', 'boot; wait 2400', None, dict(_p8_env_api2()),
              'paths8: pure math, cycle counters, texture stubs, profiler bars and layout and camera accessors by guest call in the front end (test only)')

def _p8_tok1():
    # the token reader's calls no caller used (beginScriptFromFile on a file handed over, the line, the rest of the line, the
    # line given back, the position report, the settings, a nested include pushed and popped, the state saved and restored)
    # around reading SCENE.TXT (a real script of the disc) through the game's own beginScript and nextToken, by guest call in
    # the front end (test only)
    S = 'sSCENE.TXT'
    CALLS = [('~0x14EB38',),                                          # 0: the reader's state saved
             ('~0x14E0C0', S)]                                        # reset, SCENE.TXT opened
    CALLS += [('~0x14E110', '#128', 128) for _ in range(5)]
    CALLS += [('~0x14EA08',), ('~0x14EA50',), ('~0x14EA68',), ('~0x14E110', '#128', 128), ('~0x14EA18',),
              ('~0x14EAE8', 1), ('~0x14E110', '#128', 128), ('~0x14EAE8', 0), ('~0x14EB08', 1), ('~0x14E110', '#128', 128),
              ('~0x14EB08', 0), ('~0x14EB18', 0), ('~0x14E110', '#128', 128), ('~0x14EB18', 1)]
    CALLS += [('~0x14DA08', S), ('~0x14E110', '#128', 128), ('~0x14EA18',), ('~0x14DAB8',), ('~0x14DAB8',)]
    CALLS += [('~0x14E0F0',)]                                         # closed
    CALLS += [('~0x13FA00', S, 1, 0)]                                 # a file opened by zfile
    F = '^%d' % (len(CALLS) - 1)
    CALLS += [('~0x14E048', F), ('~0x14E110', '#128', 128), ('~0x14EA68',), ('~0x14E0F0',)]
    CALLS += [('~0x14EB78', '^0')]                                    # the state put back
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_tok1():
    g = _p8_tok1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-tokapi'] = ('route', 'boot; wait 2400', None, dict(_p8_env_tok1()),
              "paths8: the token reader's file, line, settings, include and state calls by guest call around reading SCENE.TXT in the front end (test only)")

def _p8_vm1():
    # the script machine's arithmetic and shift opcodes no disc script uses (multiply, divide, or, and, shifts: with a constant and with
    # a variable of the running frame, int and float), by guest call of scriptExecute (0x1DFF28) on small programs of our own in
    # the Movie Lot (test only), each ending with a pop (JumpIfZero 0) so the machine's stack is as it was; the stack pushes
    # natives use (scriptPushResult, scriptPushFloat) and pops
    import struct
    OP = dict(Push=0x0A, Load=0x3C, Mul=0x5A, MulV=0x5B, Div=0x64, DivV=0x65, JZ=0x78, Pick=0xB4, SetType=0xBE, Or=0xC8, OrV=0xC9,
              And=0xD2, AndV=0xD3, Shl=0xDC, ShlV=0xDD, Shr=0xE6, ShrV=0xE7, Add=0x46, Sub=0x50, End=0x1F40)
    def f(x): return struct.unpack('<I', struct.pack('<f', x))[0]
    def prog(*ins):
        w = []
        for op, arg in ins: w += [OP[op], arg]
        w += [OP['End'], 0]
        return '=' + '/'.join('%#x' % x for x in w)
    P = [prog(('Push', 6), ('Mul', 7), ('Div', 3), ('Or', 0x101), ('And', 0xFF0), ('Shl', 2), ('Shr', 1), ('Pick', 1), ('JZ', 0), ('JZ', 0)),
         prog(('Push', 0x55), ('MulV', 0), ('OrV', 0), ('AndV', 0), ('ShlV', 0), ('ShrV', 0), ('JZ', 0)),
         prog(('Push', 100), ('Div', 0), ('JZ', 0)),                      # a zero divisor: the machine's own answer
         prog(('Push', 0x80000000), ('Div', 0xFFFFFFFF), ('JZ', 0)),
         prog(('Push', 0xFFFFFFF9), ('Div', 2), ('Shr', 33), ('Shl', 33), ('JZ', 0)),
         prog(('SetType', 2), ('Push', f(2.0)), ('Mul', f(3.0)), ('Div', f(4.0)), ('SetType', 1), ('JZ', 0)),
         prog(('SetType', 2), ('Push', f(2.5)), ('MulV', 0), ('SetType', 1), ('JZ', 0)),
         prog(('Push', 1000000), ('DivV', 0), ('JZ', 0)),
         prog(('SetType', 2), ('Push', f(9.0)), ('DivV', 0), ('SetType', 1), ('JZ', 0))]
    CALLS = [('~0x1DFF28', p) for p in P]
    CALLS += [('~0x1DF590', 5), ('~0x1DF5C0', 'F1.5'), ('~0x1DF500',), ('~0x1DF500',)]
    AT = 4000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_vm1():
    g = _p8_vm1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-vmops'] = ('route', 'boot; freeskate 0; skate 1', None, dict(_p8_env_vm1()),
              "paths8: the script machine's multiply, divide, or, and and shift opcodes and result pushes by guest call on programs of our own in the Movie Lot (test only)")

def _p8_io2():
    # libc's stdio over a real file: a FILE (88 bytes: _flags +0xC, _file +0xE, _cookie +0x1C, _read +0x20 .. _close +0x2C,
    # _data +0x54) made by hand around a descriptor sceOpen gives for a disc file and the stdio operations of newlib
    # 1.8.2's own (__sread, __sseek, __sclose through fread, ungetc, __submore; the _r wrappers; __smakebuf's isatty and
    # _fstat), by guest call in the front end (test only; `W`/`H` items store a word or halfword)
    RE = '%*0x2619D4'
    CALLS = [('~0x218818', 'scdrom0:\\SYSTEM.CNF;1', 1),                     # 0: the descriptor
             ('~0x138008', 0x60),                                           # 1: the FILE
             ('~0x136B40', '^1', 0, 0x60),
             ('~0x138008', 0x400),                                           # 3: a buffer
             ('H', '%(^1+0xC)', 4), ('H', '%(^1+0xE)', '^0'), ('W', '%(^1+0x1C)', '^1'), ('W', '%(^1+0x20)', 0x22B618),
             ('W', '%(^1+0x24)', 0x22B680), ('W', '%(^1+0x28)', 0x22B700), ('W', '%(^1+0x2C)', 0x22B768), ('W', '%(^1+0x54)', RE), ('W', '%(^1+0x10)', '^3'), ('W', '%(^1+0x14)', 0x400), ('W', '%(^1+0x0)', '^3')]
    CALLS += [('~0x22B420', '^1'), ('~0x229200', '#64', 1, 16, '^1'),                             # fread: the first fill (__smakebuf: isatty, _fstat)
              ('~0x229200', '#64', 1, 16, '^1'),
              ('~0x22CA28', 0x41, '^1'), ('~0x22CA28', 0x42, '^1'), ('~0x22CA28', 0x43, '^1'), ('~0x22CA28', 0x44, '^1'), ('~0x22CA28', 0x45, '^1'), ('~0x22CA28', 0x46, '^1'), ('~0x22CA28', 0x47, '^1'),   # ungetc: the second one grows the push-back buffer
              ('~0x229200', '#64', 1, 8, '^1'),
              ('~0x228E68', '^1'), ('~0x22B400', 0), ('~0x228E68', 0), ('~0x229118', RE), ('~0x229B88', RE, 0x121740),
              ('~0x22B700', '^1', 4, 0), ('~0x22B700', '^1', 0, 1), ('~0x22B700', '^1', 0, 2),
              ('~0x22B680', '^1', 's0123456789', 4),
              ('~0x22DA70', RE, '^0', '#32', 8), ('~0x22DA10', RE, '^0', 0, 0), ('~0x22D9B0', RE, '^0', '#128'),
              ('~0x22D8F8', RE, '^0', 's0123456789', 4),
              ('~0x215350', '^0'), ('~0x215358', '^0', '#128'), ('~0x215298', '^0', 0, 0),
              ('~0x22B768', '^1'),                                           # __sclose: _close_r, _close
              ('~0x218818', 'scdrom0:\\SYSTEM.CNF;1', 1),                    # a second descriptor, closed by the wrappers
              ('~0x22D958', RE, '^%d' % 0), ('~0x215288', '^0'),
              ('~0x222D30', 4, 8), ('~0x21F364',)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_io2():
    g = _p8_io2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-stdio'] = ('route', 'boot; wait 2600', None, dict(_p8_env_io2()),
              "paths8: libc stdio over a real disc file (a FILE made by hand around a sceOpen descriptor; ZX_GUEST_CALL's W/H store items) by guest call in the front end (test only)")

def _p8_part1():
    # two geometry parts made by the engine (newPart 0x1337D8, newStream 0x133E10 for positions, faces, face normals, normals,
    # uvs and colours: zero data, four vertices and faces) and the part functions no caller reached (setNormalEntry,
    # vertexColourFloats, setPartColour, setPartColourFloats, setPartUv, faceTriangle, copyFaces, shareBones, visitParts, copyStripGroups
    # with no group), by guest call in the front end (test only); the parts released at the end
    import struct
    def fs(*xs): return '=' + '/'.join('%#x' % struct.unpack('<I', struct.pack('<f', x))[0] for x in xs)
    CALLS = [('~0x1337D8',), ('~0x1337D8',)]                                       # 0, 1
    for part in (0, 1):
        P = '^%d' % part
        for kind, nbytes in ((0, 64), (1, 32), (2, 48), (3, 48), (4, 16), (5, 16)):
            CALLS.append(('~0x133E10', P, kind, nbytes, 0))
        CALLS += [('H', '%%(^%d+0x34)' % part, 4), ('H', '%%(^%d+0x36)' % part, 4)]
    A, B = '^0', '^1'
    CALLS += [('~0x134A48', A, 1, fs(0.0, 1.0, 0.0)), ('~0x134B50', A, 2, '#16'), ('~0x134B28', A, 1, '=0x11223344'),
              ('~0x134C00', A, 2, fs(0.5, 0.25, 1.0, 0.0)), ('~0x134D28', A, 3, fs(0.5, 0.25)), ('~0x1348C8', A, 1, '#64'),
              ('~0x134F50', A, B, 2, 0, 1, 1), ('~0x134F50', A, B, 2, 1, 0, 0),
              ('~0x134E58', 0x100, 0x200, 0, 0, 0),
              ('~0x134700', A, B), ('~0x134D80',),
              ('~0x133838', A), ('~0x133838', B)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_part1():
    g = _p8_part1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-partapi'] = ('route', 'boot; wait 2600', None, dict(_p8_env_part1()),
              'paths8: geometry parts made by the engine and the part and stream functions no caller reached, by guest call in the front end (test only)')

def _p8_emit1():
    # a particle emitter scene object made by the engine's maker (newParticleEmitterSceneObject 0x15EDC8: no arguments), placed,
    # turned and scaled through the object API (the class's hooks), found by id, left to the frame task (updateParticleEmitterObject),
    # (not deleted: its destroy hook calls a null function when the emitter has no effect), by guest call in the front end (test only)
    import struct
    def fs(*xs): return '=' + '/'.join('%#x' % struct.unpack('<I', struct.pack('<f', x))[0] for x in xs)
    CALLS = [('~0x15EDC8',),
             ('~0x13BB90', '^0', fs(1.0, 2.0, 3.0, 1.0)), ('~0x13BC38', '^0', fs(0.0, 0.0, 0.0, 1.0)), ('~0x13BBF8', '^0', fs(2.0, 2.0, 2.0, 1.0)),
             ('~0x13BEF8', 0x12345, 0, 0), ('~0x13BEF8', 0, 0, 0),
             ]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_emit1():
    g = _p8_emit1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-emitter'] = ('route', 'boot; wait 2600', None, dict(_p8_env_emit1()),
              'paths8: a particle emitter scene object made, placed and found by guest call in the front end (test only; no asset has an emitter)')

def _p8_api3():
    # libvu0's clamp, rotationFromTo (the matrix turning one vector onto another), a bounding box union, the keyed-entry
    # search callbacks (matchKeyCallback, findKeyedEntryByValue over the animation table), by guest call in the Movie Lot's
    # free skate (the animation table is loaded there; test only)
    import struct
    def fs(*xs): return '=' + '/'.join('%#x' % struct.unpack('<I', struct.pack('<f', x))[0] for x in xs)
    CALLS = [('~0x212D10', '#16', fs(-5.0, 0.5, 9.0, 1.0), 0, 0, 0, 0, 'F-1.0', 'F1.0'),
             ('~0x212D10', '#16', fs(2.0, -2.0, 0.0, 1.0), 0, 0, 0, 0, 'F0.0', 'F0.5'),
             ('~0x13A340', '#64', fs(1, 0, 0, 0), fs(0, 1, 0, 0)), ('~0x13A340', '#64', fs(1, 0, 0, 0), fs(1, 0, 0, 0)),
             ('~0x13A340', '#64', fs(1, 0, 0, 0), fs(-1, 0, 0, 0)),
             ('~0x151630', '#32', fs(0, 0, 0, 0, 1, 1, 1, 0), fs(-1, 0.5, 2, 0, 0.5, 3, 4, 0)),
             ('~0x163F88', 0x55, 0x66, '=0x55/0'), ('~0x163F88', 0x56, 0x66, '=0x55/0'),
             ('~0x1AE500', '=0/0/0/0x7', '=0/0x7/0'), ('~0x1AE500', '=0/0/0/0x8', '=0/0x7/0'),
             ('~0x1AE490', 0x7, '#16'), ('~0x1AE490', 0x12345678, '#16')]
    AT = 4000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_api3():
    g = _p8_api3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-api3'] = ('route', 'boot; freeskate 0; skate 1', None, dict(_p8_env_api3()),
              'paths8: libvu0 clamp, rotationFromTo, a box union and the keyed-entry search callbacks by guest call in the Movie Lot (test only)')

def _p9_skey2():
    # sampleScaleKey on a scale track of two keys made in a heap block (count 2 at +0, the keys' address at +8; a key is a
    # halfword time in 1/128 s and three signed halfword scales in 1/128), by guest call in the front end (test only): a
    # time before the first key, between the keys, on the last key's and past it. (An empty track, as paths8 called it,
    # makes the original read the word before a null keys pointer: an address error, EXL set at the exit.)
    B = '%(^0+{})'
    CALLS = [('~0x138008', 0x40),
             ('W', B.format(0), 2), ('W', B.format(8), '%(^0+0x20)'),
             ('W', B.format(0x20), 0 | 128 << 16), ('W', B.format(0x24), 128 | 128 << 16),
             ('W', B.format(0x28), 128 | 192 << 16), ('W', B.format(0x2C), 256 | 64 << 16),
             ('~0x1414F8', '^0', '#16', 'F-1.0'), ('~0x1414F8', '^0', '#16', 'F0.5'), ('~0x1414F8', '^0', '#16', 'F1.0'),
             ('~0x1414F8', '^0', '#16', 'F5.0'), ('~0x1414F8', '^0', '#16', 'F0.0')]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_skey2():
    g = _p9_skey2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-scalekey'] = ('route', 'boot; wait 3400', None, dict(_p8_env_skey2()),
              'paths9: sampleScaleKey on a scale track of two keys made in a heap block, by guest call in the front end (test only)')

def _p9_free1():
    # freeAnimator (0x1454E8) on animators made by hand in engine heap blocks (test only; by guest call in the front end):
    # a null animator, one with no layers, and one with two layers, the first holding two animation nodes (made by the
    # engine's node maker 0x141208, linked head -> second) and the second none. (cloneAnimator 0x1451F8 never sets the
    # clone's list head, which keeps the original's first node: freeing a clone returns the ORIGINAL's nodes to the pool, so the skater's
    # animator ran on freed nodes in paths8's cov-animq; a clone is not freed here.)
    A = '%(^0+{})'
    N1 = '%(^1+{})'
    N2 = '%(^2+{})'
    CALLS = [('~0x138008', 0x140), ('~0x141208',), ('~0x141208',), ('~0x138008', 0x40),
             ('W', A.format(0), 2), ('W', A.format(8), '%(^0+0x20)'),
             ('W', A.format(0x98), '^1'), ('W', A.format(0x128), 0),
             ('W', N1.format(0x40), '^2'), ('W', N1.format(0x44), 0), ('W', N2.format(0x40), 0), ('W', N2.format(0x44), '^1'),
             ('W', '%(^3+0)', 0), ('W', '%(^3+8)', 0),
             ('~0x1454E8', 0), ('~0x1454E8', '^3'), ('~0x1454E8', '^0')]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p8_env_free1():
    g = _p9_free1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-animfree'] = ('route', 'boot; wait 3400', None, dict(_p8_env_free1()),
              'paths9: freeAnimator on animators made by hand in heap blocks (none, no layers, two layers with linked animation nodes), by guest call in the front end (test only)')

def _p9_wt1():
    # Wall Tap (state 13) entered by setSkaterState from the update of a Wall Ride (the lesson), by guest call (test only)
    ENV = {'ZX_GUEST_CALL': '0x1A2F18@300:~0x1A42E0:%@4:13'}
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_wt1():
    g = _p9_wt1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-walltap'] = ('route', 'boot; tutorial; wallride; walljump', None, dict(FRONT_END_SKIPPED, **_p9_env_wt1()),
              "paths9: the Tutorial's wall ride lesson with the Wall Tap state (13) entered by guest call from the ride's update (test only)")

def _p9_sound1():
    # soundEvent (0x1C2558: one function every sound goes through; 81 events) called by guest call from the frame hook in
    # the Movie Lot's free skate (test only): every event number with the arguments it takes (a position is the skater's,
    # P), the sources it starts stopped and moved again, volumes and tracks set, and numbers past the table.
    P = '%(*(*0x24C9BC+0x10)+0xA0)'
    E = '~0x1C2558'
    L = []
    def ev(*a): L.append((E,) + a)
    for k in (0, 1, 2, 3):
        ev(0, k, 100, 1); ev(0, k, 127, 0)
    for k in (0, 1, 2): ev(1, k)
    ev(2, 0, 0); ev(3); ev(4, 5); ev(4, 13); ev(4, 20); ev(5)
    for t in (1, 2, 20): ev(6, t)
    ev(8, 1); ev(8, 20); ev(7, 1); ev(7, 20); ev(9, 0); ev(10, 0); ev(9, 20); ev(11)
    for k in range(13, 19): ev(k)
    for k in (19, 20, 21, 22, 26, 28, 54, 59, 60, 61, 67, 68, 69, 70, 72): ev(k)
    ev(23); ev(24, 0, 0); ev(24, 1, 0); ev(24, 0, 9999)
    for k in (25, 27, 29): ev(k, 0, 0, 64); ev(k, 0, 5, 127)
    ev(30, 0, 0, 5); ev(30, 0, 3, 100)
    ev(31, P, 0, 5); ev(31, P, 0, 0)
    for b in (0, 1, 5): ev(33, P, b)
    ev(33, P, 0)
    for k in (32, 34, 35, 36, 37, 38): ev(k, P, 0); ev(k, P, 1)
    for k in (39, 40, 41): ev(k, P)
    ev(42, 0); ev(42, 3); ev(43, 0); ev(43, 2); ev(44, 0); ev(44, 1); ev(44, 2); ev(45)
    for k in (46, 63, 64, 65, 66, 71, 73, 74, 75, 76, 77, 78): ev(k, 0)
    ev(47, P, 0, 100); ev(47, P, 9999, 100)
    ev(48, 0); ev(48, 9999); ev(49, P); ev(51)
    ev(50, 0xFFFF, 100)
    ev(52, P, 0, 100); ev(52, P, 9999, 100)
    ev(53, 0); ev(53, 1); ev(53, 2)
    n = len(L)
    ev(56, P, 0, 0, 100)                  # a source started
    ev(56, P, 2, '^%d' % n, 100); ev(56, P, 3, 0, 100); ev(56, P, 1, '^%d' % n); ev(56, P, 1, 0xFFFF); ev(56, P, 2, 0xFFFF, 1)
    ev(56, P, 9, 0, 0); ev(57, P, 0, 1, 50); ev(57, P, 3, 1, 50)
    ev(58, 0); ev(58, 5); ev(58, 20); ev(62)
    ev(79, 0); ev(80, 0, 1); ev(80, 0, 1)
    ev(81); ev(200)
    ev(55, P); ev(55, P)
    CALLS = L
    AT = 4300; PER = 4
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_sound1():
    g = _p9_sound1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-soundevents'] = ('route', 'boot; freeskate 0; skate 2', None, dict(_p9_env_sound1()),
              "paths9: soundEvent (every event 0 to 80 with its arguments, sources started, stopped and moved, volumes and tracks set, numbers past the table) by guest call in the Movie Lot's free skate (test only)")

def _p9_colq1():
    # collisionQuery (0x125BE0) kinds the game never asks for: a box (2) and a box swept along a segment (3), and the
    # query cache (a QueryCache in a heap block) on every kind: a fresh query, the same one answered from the cache,
    # a shape moved past the limit, other flags, the cache's own limits, and the faces drawn; around the skater where
    # the Movie Lot's free skate starts (302.18, -10.85, -155.12), by guest call (test only).
    import struct
    f = lambda x: struct.unpack('<I', struct.pack('<f', x))[0]
    Q = '~0x125BE0'
    SB, SS, SW, SSW = 0, 0x40, 0x80, 0xC0       # shape offsets in block S: box, sphere, segment, swept box
    S = '%(^0+{})'
    def vec(base, off, x, y, z, w=0.0):
        return [('W', S.format(base + off + 4 * i), f(v)) for i, v in enumerate((x, y, z, w))]
    CALLS = [('~0x138008', 0x100), ('~0x138008', 0x300), ('~0x138008', 0x300), ('~0x138008', 0x300), ('~0x138008', 0x300)]
    CALLS += vec(SB, 0, 299, -14, -158) + vec(SB, 0x10, 305, -8, -152)
    CALLS += vec(SS, 0, 302.2, -10.5, -155.1) + [('W', S.format(SS + 0x10), f(2.5))]
    CALLS += vec(SW, 0, 302.2, -14, -155.1) + vec(SW, 0x10, 302.2, -6, -155.1)
    CALLS += vec(SSW, 0, 299, -10.5, -155.1) + vec(SSW, 0x10, 305, -10.5, -155.1) + vec(SSW, 0x20, 1.5, 0, 0)
    for c in (3, 4):
        CALLS += [('W', '%%(^%d+%d)' % (c, o), 0) for o in range(0, 0x20, 4)]           # a cache not valid
    CALLS += [('W', '%(^4+0x14)', f(0.5)), ('W', '%(^4+0x18)', f(0.1)), ('W', '%(^4+0x1C)', 50)]
    SH = lambda off: '%%(^0+%d)' % off
    R, C, C2 = '^1', '^3', '^4'
    # (kind, shape, flags, filter, context, nodes, results, cache)
    CALLS += [(Q, 2, SH(SB), 0, 0, 0, 0, R, 0), (Q, 3, SH(SSW), 0, 0, 0, 0, R, 0), (Q, 0, SH(SS), 0, 0, 0, 0, R, 0),
              (Q, 1, SH(SW), 4, 0, 0, 0, R, 0), (Q, 2, SH(SB), 0x84, 0, 0, 0, R, 0), (Q, 3, SH(SSW), 0x84, 0, 0, 0, R, 0)]
    for kind, off in ((2, SB), (3, SSW), (0, SS), (1, SW)):
        CALLS += [(Q, kind, SH(off), 4, 0, 0, 0, R, C), (Q, kind, SH(off), 4, 0, 0, 0, R, C), (Q, kind, SH(off), 4, 0, 0, 0, 0, C),
                  (Q, kind, SH(off), 0x24, 0, 0, 0, R, C), (Q, kind, SH(off), 0x84, 0, 0, 0, R, C),
                  (Q, kind, SH(off), 0x1C04, 0, 0, 0, R, C), (Q, kind, SH(off), 0x1C04, 0, 0, 0, R, C),
                  (Q, kind, SH(off), 0xE404, 0, 0, 0, R, C), (Q, kind, SH(off), 0xA404, 0, 0, 0, R, C),
                  (Q, kind, SH(off), 0x44, 0, 0, 0, R, C2), (Q, kind, SH(off), 0x44, 0, 0, 0, R, C2),
                  (Q, kind, SH(off), 0x2000, 0, 0, 0, R, C2), (Q, kind, SH(off), 0x4400, 0, 0, 0, R, C2)]
        CALLS += [('W', '%%(^0+%d)' % off, f(400.0)),                    # the shape's first word moved far
                  (Q, kind, SH(off), 4, 0, 0, 0, R, C), (Q, kind, SH(off), 0x44, 0, 0, 0, R, C2),
                  ('W', '%%(^0+%d)' % off, f(299.0 if off == SB else 302.2 if off in (SW, SS) else 299.0))]
    AT = 2300; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_colq1():
    g = _p9_colq1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-colquery'] = ('route', 'boot; freeskate 0; wait 700', None, dict(_p9_env_colq1()),
              "paths9: collisionQuery boxes, swept boxes and the query cache on every kind (flags, limits, drawn hits) around the skater's start in the Movie Lot's free skate, by guest call (test only)")

def _p9_lib3():
    # libm's and libgcc's rarer paths, by guest call in the front end (test only): sinf, cosf and tanf on arguments of
    # every size past 2**7 pi/2 (__ieee754_rem_pio2f's large-argument reduction and __kernel_rem_pio2f's loops, each
    # exponent), infinities and NaNs; atan2f, atanf and sqrtf on every quadrant, zero, infinite and NaN input; the soft
    # double-to-integer (dptoli) and the 64-bit unsigned division and remainder (libgcc) on operands of every size.
    import struct
    inf, nan = float('inf'), float('nan')
    CALLS = []
    xs = []
    for k in range(7, 128, 5):
        xs += [1.2345678 * 2.0 ** k, -1.7654321 * 2.0 ** k]
    xs += [1e38, -1e38, 3.4e38, inf, -inf, nan, 100.0, 6.5e4, 1.0e5, 1.0e7, 8.3e8]
    for x in xs:
        CALLS += [('~0x202750', 'F%r' % x), ('~0x202560', 'F%r' % x), ('~0x202840', 'F%r' % x)]    # sinf, cosf, tanf
    for y in (0.0, -0.0, 1.0, -1.0, 1e30, -1e30, 1e-30, inf, -inf, nan):
        for x in (0.0, -0.0, 1.0, -1.0, 1e30, -1e30, 1e-30, inf, -inf, nan):
            CALLS += [('~0x202AC8', 'F%r' % y, 'F%r' % x)]                                        # atan2f(y, x)
    for x in (0.0, -0.0, 0.1, 0.3, 0.45, 0.6, 0.9, 1.0, 1.2, 2.0, 2.5, 6.0, 100.0, 1e10, 1e30, -0.3, -2.5, -100.0, -1e30, inf, -inf, nan, 1e-30):
        CALLS += [('~0x2021E0', 'F%r' % x)]                                                       # atanf
    for x in (0.0, -0.0, 1.0, 2.0, 4.0, 1e-30, 1e-42, -1.0, -4.0, inf, -inf, nan, 3.0e38, 1e-45):
        CALLS += [('~0x203A88', 'F%r' % x)]                                                       # __ieee754_sqrtf
    def d(x): return struct.unpack('<Q', struct.pack('<d', x))[0]
    for x in (0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 2.5, 1e10, -1e10, 4294967296.5, 9.2e18, -9.2e18, 9.3e18, -9.3e18, 1e30, -1e30,
              inf, -inf, nan, 5e-324, 0.99999, 123456789012.75):
        CALLS += [('~0x221F58', 'L0x%X' % d(x))]                                                  # dptoli
    M = 0xFFFFFFFFFFFFFFFF
    ns = (0, 1, 7, 100, 0xFFFFFFFF, 0x100000000, 0x100000001, 0x123456789ABCDEF0, 0x8000000000000000, M, 0xFFFFFFFE00000001, 0x00000001FFFFFFFF)
    ds = (1, 2, 3, 10, 0xFFFF, 0x10000, 0xFFFFFFFF, 0x100000000, 0x100000001, 0x123456789, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, M)
    for n in ns:
        for dv in ds:
            CALLS += [('~0x220948', 'L0x%X' % n, 'L0x%X' % dv), ('~0x220F18', 'L0x%X' % n, 'L0x%X' % dv)]   # __udivdi3, __umoddi3
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_lib3():
    g = _p9_lib3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-libm3'] = ('route', 'boot; wait 3400', None, dict(_p9_env_lib3()),
              "paths9: libm's large-argument reductions, atan2f/atanf/sqrtf on every quadrant and special value, dptoli, and libgcc's 64-bit division and remainder on operands of every size, by guest call in the front end (test only)")

def _p9_sound2():
    # soundEvent in the front end (game mode 1: menu sounds and music), every event as spec sound1 does in a level (test only).
    # the Movie Lot's free skate (test only): every event number with the arguments it takes (a position is the skater's,
    # P), the sources it starts stopped and moved again, volumes and tracks set, and numbers past the table.
    P = '0x2A76F0'
    E = '~0x1C2558'
    L = []
    def ev(*a): L.append((E,) + a)
    for k in (0, 1, 2, 3):
        ev(0, k, 100, 1); ev(0, k, 127, 0)
    for k in (0, 1, 2): ev(1, k)
    ev(2, 0, 0); ev(3); ev(4, 5); ev(4, 13); ev(4, 20); ev(5)
    for t in (1, 2, 20): ev(6, t)
    ev(8, 1); ev(8, 20); ev(7, 1); ev(7, 20); ev(9, 0); ev(10, 0); ev(9, 20); ev(11)
    for k in range(13, 19): ev(k)
    for k in (19, 20, 21, 22, 26, 28, 54, 59, 60, 61, 67, 68, 69, 70, 72): ev(k)
    ev(23); ev(24, 0, 0); ev(24, 1, 0); ev(24, 0, 9999)
    for k in (25, 27, 29): ev(k, 0, 0, 64); ev(k, 0, 5, 127)
    ev(30, 0, 0, 5); ev(30, 0, 3, 100)
    ev(31, P, 0, 5); ev(31, P, 0, 0)
    for b in (0, 1, 5): ev(33, P, b)
    ev(33, P, 0)
    for k in (32, 34, 35, 36, 37, 38): ev(k, P, 0); ev(k, P, 1)
    for k in (39, 40, 41): ev(k, P)
    ev(42, 0); ev(42, 3); ev(43, 0); ev(43, 2); ev(44, 0); ev(44, 1); ev(44, 2); ev(45)
    for k in (46, 63, 64, 65, 66, 71, 73, 74, 75, 76, 77, 78): ev(k, 0)
    ev(47, P, 0, 100); ev(47, P, 9999, 100)
    ev(48, 0); ev(48, 9999); ev(49, P); ev(51)
    ev(50, 0xFFFF, 100)
    ev(52, P, 0, 100); ev(52, P, 9999, 100)
    ev(53, 0); ev(53, 1); ev(53, 2)
    n = len(L)
    ev(56, P, 0, 0, 100)                  # a source started
    ev(56, P, 2, '^%d' % n, 100); ev(56, P, 3, 0, 100); ev(56, P, 1, '^%d' % n); ev(56, P, 1, 0xFFFF); ev(56, P, 2, 0xFFFF, 1)
    ev(56, P, 9, 0, 0); ev(57, P, 0, 1, 50); ev(57, P, 3, 1, 50)
    ev(58, 0); ev(58, 5); ev(58, 20); ev(62)
    ev(79, 0); ev(80, 0, 1); ev(80, 0, 1)
    ev(81); ev(200)
    ev(55, P); ev(55, P)
    CALLS = L
    AT = 1700; PER = 4
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_sound2():
    g = _p9_sound2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-soundfront'] = ('route', 'boot; wait 3000', None, dict(_p9_env_sound2()),
              'paths9: soundEvent in the front end (every event 0 to 80 in game mode 1), by guest call (test only)')

def _p9_misc1():
    # Engine calls that no code of the game makes (no jal, no table word), made by guest call in the Movie Lot's free skate
    # (test only), each on arguments it can take: the stubs and hooks the engine's tables hold none of; accessors on a zeroed
    # heap block; the TGA reader on headers made by hand (isTgaImage, readTgaImage); the objects' replay state sized and
    # saved (objectsReplaySize, saveObjectsReplayState; not restored: restoreObjectsReplayState loops for ever on a count
    # of 5 or more and does nothing below it), the replay-mode switch nextReplayMode left to 0.
    H, H2, F, O = '^0', '^1', '^2', '^3'
    HB = '%(^0+{})'
    FB = '%(^2+{})'
    CALLS = [('~0x138008', 0x100), ('~0x138008', 0x100), ('~0x138008', 0x100), ('~0x138008', 0x40),
             ('~0x136B40', H, 0, 0x100), ('~0x136B40', H2, 0, 0x100), ('~0x136B40', F, 0, 0x100), ('~0x136B40', O, 0, 0x40)]
    for a in (0x138438, 0x10A678, 0x108B90, 0x14C4E8, 0x1665C8, 0x1B52A8, 0x1C1710, 0x1CB118, 0x1D76F8, 0x1D7F38, 0x1D9428,
              0x1DA740, 0x1F46B0, 0x1F46D0, 0x1F7C80, 0x103228, 0x1DAB50, 0x14CA60, 0x1D8938, 0x1BE468, 0x1BE470):
        CALLS.append(('~0x%X' % a,))
    CALLS += [('~0x165D58', H), ('~0x165D68', H), ('~0x1DED78', H), ('~0x1F5A80', H), ('~0x1F5A78', H, 5), ('~0x1F5A80', H),
              ('~0x165FC0', H, 7), ('~0x1FA510', H), ('~0x1FA390', H, H2), ('~0x1FA510', H), ('~0x133228', H), ('~0x133228', H),
              ('W', HB.format(0x78), '^1'), ('~0x118FC8', H)]
    # TGA: a 4x4 32-bit image bottom-left origin (header word 3 is width | height << 16, word 4 bpp | descriptor << 8), the same
    # top-left (descriptor 0x20), right-origin (0x10), 24- and 16-bit, a width that is no power of two, 8 bits (no TGA), and a BMP
    def tga(w, h, bpp, desc, name):
        return [('W', FB.format(0), 0x00020000), ('W', FB.format(12), w | h << 16), ('W', FB.format(16), bpp | desc << 8)]
    for (w, h, bpp, desc) in ((4, 4, 32, 0), (4, 4, 32, 0x20), (4, 4, 32, 0x10), (4, 8, 24, 0), (8, 2, 16, 0x20), (3, 4, 32, 0),
                              (4, 3, 32, 0), (4, 4, 8, 0)):
        CALLS += tga(w, h, bpp, desc, '') + [('~0x127FB8', F)]
        if w == 4 and h in (4, 8) or (w, h) == (8, 2):
            CALLS += [('~0x127DA8', O, F)]
    CALLS += [('W', FB.format(0), 0x4D42), ('~0x127FB8', F)]
    # the replay state of the objects
    CALLS += [('~0x1DAB68',)]
    n = len(CALLS) - 1
    CALLS += [('~0x138008', '%%(^%d+0x40)' % n), ('~0x1DAB98', '^%d' % (n + 1)), ('~0x1DABE8', '^%d' % (n + 1), 3), ('~0x1DACA8',)]
    # state setters, last
    CALLS += [('~0x117FC0',), ('~0x10F088',)]
    AT = 2300; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_misc1():
    g = _p9_misc1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-misc1'] = ('route', 'boot; freeskate 0; wait 400', None, dict(_p9_env_misc1()),
              "paths9: engine stubs, accessors and the TGA reader that no code calls, the objects' replay state sized and saved, by guest call in the Movie Lot's free skate (test only)")

def _p9_misc2():
    # More engine and game methods that only a table slot or a rare branch calls, made by guest call in the Movie Lot's free
    # skate (test only) on zeroed heap blocks standing for the object: the follower, breakable, trick-text, skitch and
    # traffic-car message stubs, the sound-stream stub, takeTranslation, a list append, the frame-seconds getter, and the
    # animation sound (soundEvent 51).
    H, H2 = '^0', '^1'
    HB = '%(^0+{})'
    CALLS = [('~0x138008', 0x100), ('~0x138008', 0x100), ('~0x136B40', H, 0, 0x100), ('~0x136B40', H2, 0, 0x100),
             ('W', HB.format(4), '^1'), ('W', HB.format(0x14), '^1'), ('W', HB.format(0x8D8), 77), ('W', HB.format(0x58), 9)]
    CALLS += [('~0x113F78',), ('~0x165D38', H), ('~0x1AB068',), ('~0x1B50B8', H), ('~0x1E61C0', H, 5), ('~0x1EAF58',), ('~0x1EAF60',),
              ('~0x1EB170',), ('~0x1F1C88', H, 3), ('~0x1F1C90', H), ('~0x1F7D30',), ('~0x1F9BF0',), ('~0x1FA670',), ('~0x137BE0',),
              ('~0x1ED790', H), ('~0x1ED7A0', H), ('~0x1E48A8', H), ('~0x190590',), ('~0x117FF0', H, H2),
              ('~0x15CC58', H, 7), ('~0x15CC58', H, 8)]
    AT = 2300; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_misc2():
    g = _p9_misc2()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-misc2'] = ('route', 'boot; freeskate 0; wait 400', None, dict(_p9_env_misc2()),
              "paths9: method stubs, a getter, takeTranslation, a list append and the animation sound on zeroed heap blocks standing for their objects, by guest call in the Movie Lot's free skate (test only)")

# p9 begin cov-mcsave
ROUTES['cov-mcsave-01'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Mkdir:1:-3'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Mkdir:1:-3]')
ROUTES['cov-mcsave-02'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Mkdir:1:-5'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Mkdir:1:-5]')
ROUTES['cov-mcsave-03'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Open:1:-5'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Open:1:-5]')
ROUTES['cov-mcsave-04'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Open:2:-3'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Open:2:-3]')
ROUTES['cov-mcsave-05'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Open:3:-3'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Open:3:-3]')
ROUTES['cov-mcsave-06'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Write:1:-3'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Write:1:-3]')
ROUTES['cov-mcsave-07'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Write:3:-5'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Write:3:-5]')
ROUTES['cov-mcsave-08'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Write:4:-3'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Write:4:-3]')
ROUTES['cov-mcsave-09'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Close:3:-5'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Close:3:-5]')
ROUTES['cov-mcsave-10'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Chdir:9:-4'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Chdir:9:-4]')
ROUTES['cov-mcsave-11'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='GetInfo:292:-10'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=GetInfo:292:-10]')
ROUTES['cov-mcsave-12'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='GetInfo:292:-2'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=GetInfo:292:-2]')
ROUTES['cov-mcsave-13'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='GetInfo:293:-1'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=GetInfo:293:-1]')
ROUTES['cov-mcsave-14'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='GetInfo:150:-10'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=GetInfo:150:-10]')
ROUTES['cov-mcsave-15'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Open:4:-4'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Open:4:-4]')
ROUTES['cov-mcsave-16'] = ('route', 'boot; cardsave', None, dict(FRONT_END_SKIPPED, ZX_MC_FAIL='Flush:1:-5'),
    'paths9: the career saved to the memory card from OPTIONS > SAVE & LOAD with libmc commands failing partway (ZX_MC_FAIL, test only) [ZX_MC_FAIL=Flush:1:-5]')
# p9 end cov-mcsave

# p9 begin cov-mcdlg
ROUTES['cov-mcdlg-01'] = ('route', 'boot; carddialog save 0 DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-02'] = ('route', 'boot; carddialog save 0 CROSS DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 CROSS DOWN+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-03'] = ('route', 'boot; carddialog save 0 TRIANGLE', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 TRIANGLE; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-04'] = ('route', 'boot; carddialog save 0 DOWN+DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+DOWN+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-05'] = ('route', 'boot; carddialog save 0 UP+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 UP+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-06'] = ('route', 'boot; carddialog save 0 LEFT+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 LEFT+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-07'] = ('route', 'boot; carddialog save 0 RIGHT+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 RIGHT+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-08'] = ('route', 'boot; carddialog save 1 DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT2='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 1 DOWN+CROSS; ZX_MC_SLOT2=unformatted]')
ROUTES['cov-mcdlg-09'] = ('route', 'boot; carddialog save 1 CROSS TRIANGLE', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT2='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 1 CROSS TRIANGLE; ZX_MC_SLOT2=unformatted]')
ROUTES['cov-mcdlg-10'] = ('route', 'boot; carddialog save 0 CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 CROSS; ZX_MC_SLOT1=none]')
ROUTES['cov-mcdlg-11'] = ('route', 'boot; carddialog save 0 TRIANGLE', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 TRIANGLE; ZX_MC_SLOT1=none]')
ROUTES['cov-mcdlg-12'] = ('route', 'boot; carddialog save 0 DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS; ZX_MC_SLOT1=none]')
ROUTES['cov-mcdlg-13'] = ('route', 'boot; carddialog save 1 CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT2='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 1 CROSS; ZX_MC_SLOT2=none]')
ROUTES['cov-mcdlg-14'] = ('route', 'boot; carddialog load 0 CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog load 0 CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-15'] = ('route', 'boot; carddialog load 0 DOWN+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog load 0 DOWN+CROSS; ZX_MC_SLOT1=unformatted]')
ROUTES['cov-mcdlg-16'] = ('route', 'boot; carddialog load 0 TRIANGLE', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog load 0 TRIANGLE; ZX_MC_SLOT1=none]')
ROUTES['cov-mcdlg-17'] = ('route', 'boot; carddialog load 1 CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT2='none'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog load 1 CROSS; ZX_MC_SLOT2=none]')
ROUTES['cov-mcdlg-18'] = ('route', 'boot; carddialog save 0 DOWN+CROSS UP+CROSS UP+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted', ZX_MC_FAIL='Format:1:-5'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS UP+CROSS UP+CROSS; ZX_MC_SLOT1=unformatted, ZX_MC_FAIL=Format:1:-5]')
ROUTES['cov-mcdlg-19'] = ('route', 'boot; carddialog save 0 DOWN+CROSS UP+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted', ZX_MC_FAIL='Format:1:-3'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS UP+CROSS; ZX_MC_SLOT1=unformatted, ZX_MC_FAIL=Format:1:-3]')
ROUTES['cov-mcdlg-20'] = ('route', 'boot; carddialog save 0 DOWN+CROSS UP+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted', ZX_MC_FAIL='Mkdir:1:-3'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS UP+CROSS; ZX_MC_SLOT1=unformatted, ZX_MC_FAIL=Mkdir:1:-3]')
ROUTES['cov-mcdlg-21'] = ('route', 'boot; carddialog save 0 DOWN+CROSS UP+CROSS', None, dict(FRONT_END_SKIPPED, ZX_MC_SLOT1='unformatted', ZX_MC_FAIL='Open:3:-5'),
    'paths9: the memory card dialogs of OPTIONS > SAVE & LOAD with an unformatted or missing card (ZX_MC_SLOT1/2) and failing commands (ZX_MC_FAIL), test only [boot; carddialog save 0 DOWN+CROSS UP+CROSS; ZX_MC_SLOT1=unformatted, ZX_MC_FAIL=Open:3:-5]')
# p9 end cov-mcdlg


# p9 begin cov-animload
def _p9_anim3():
    # The scene database's animation loaders no caller reaches (loadAnimationFile for the ANIM kind's NAD, loadAnimationLibrary for a
    # packed library, loadLibraryModel for a MODEL's ZAL), on real disc files named in a request made in a heap block ({file, motions}),
    # the motions listed, found by id and by index, and released: the loaded-asset record made by hand ({entry 0, id 0, kind, form 2,
    # asset the motion list}); by guest call in the front end (test only)
    R = '%(^{}+{})'
    CALLS = [('~0x138008', 0x40), ('~0x138008', 0x100), ('~0x138008', 0x40), ('~0x138008', 0x100), ('~0x138008', 0x40),
             ('~0x2249B4', '^1', 'sassets\\npc\\wave.nad'),
             ('W', R.format(0, 0), '^1'), ('W', R.format(0, 4), 0),
             ('~0x13D1D8', '^0')]                                        # 5..: loadAnimationFile
    CALLS += [('W', R.format(2, 0x08), 0x202), ('W', R.format(2, 0x0C), '%*(^0+4)'), ('W', R.format(2, 0), 0), ('W', R.format(2, 4), 0)]
    CALLS += [('~0x13DA10', '^2', '%**(^0+4)'), ('~0x13DA10', '^2', 0x12345), ('~0x13DA10', 0, 5), ('~0x13DA58', '^2', 0), ('~0x13DA58', '^2', 99),
              ('~0x13DA58', 0, 0), ('~0x13DA90', '^2', '^3', 64), ('~0x13DA90', '^2', '^3', 0), ('~0x13DA90', '^2', '^3', 1)]
    # a second library in the record: more motions from another file, then released through releaseAnimations (frees the motions and the record's entry)
    CALLS += [('~0x2249B4', '^1', 'sassets\\npc\\loiter.nad'), ('W', R.format(4, 0), '^1'), ('W', R.format(4, 4), 0), ('~0x13D1D8', '^4'),
              ('~0x2249B4', '^1', 'sassets\\npc\\nosuch.nad'), ('W', R.format(4, 0), '^1'), ('W', R.format(4, 4), 0), ('~0x13D1D8', '^4'),
              ('~0x2249B4', '^1', 'sassets\\uilib.zal'), ('W', R.format(4, 0), '^1'), ('W', R.format(4, 4), 0), ('~0x13D260', '^4'),
              ('~0x2249B4', '^1', 'sassets\\nosuch.zal'), ('W', R.format(4, 0), '^1'), ('W', R.format(4, 4), 0), ('~0x13D260', '^4'),
              ('~0x2249B4', '^1', 'sassets\\fx.zal'), ('W', R.format(4, 0), '^1'), ('W', R.format(4, 4), 5), ('~0x13D0B0', '^4'),
              ('~0x2249B4', '^1', 'sassets\\nosuch.zal'), ('W', R.format(4, 0), '^1'), ('~0x13D0B0', '^4'),
              ('~0x13D988', 0), ('~0x13D988', '^2')]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_anim3():
    g = _p9_anim3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-animload'] = ('route', 'boot; wait 3000', None, dict(_p9_env_anim3()),
              "paths9: the scene database's animation loaders (NAD file, packed library, library model) on real disc files, the motions listed, found, indexed and released, by guest call in the front end (test only)")
# p9 end cov-animload

# p9 begin cov-soundpause
def _p9_sound3():
    # soundEvent while the game is paused (game mode 3: events 25 to 54 answered 0, event 55 the pause), every event as spec sound1 does (test only).
    # the Movie Lot's free skate (test only): every event number with the arguments it takes (a position is the skater's,
    # P), the sources it starts stopped and moved again, volumes and tracks set, and numbers past the table.
    P = '%(*(*0x24C9BC+0x10)+0xA0)'
    E = '~0x1C2558'
    L = []
    def ev(*a): L.append((E,) + a)
    for k in (0, 1, 2, 3):
        ev(0, k, 100, 1); ev(0, k, 127, 0)
    for k in (0, 1, 2): ev(1, k)
    ev(2, 0, 0); ev(3); ev(4, 5); ev(4, 13); ev(4, 20); ev(5)
    for t in (1, 2, 20): ev(6, t)
    ev(8, 1); ev(8, 20); ev(7, 1); ev(7, 20); ev(9, 0); ev(10, 0); ev(9, 20); ev(11)
    for k in range(13, 19): ev(k)
    for k in (19, 20, 21, 22, 26, 28, 54, 59, 60, 61, 67, 68, 69, 70, 72): ev(k)
    ev(23); ev(24, 0, 0); ev(24, 1, 0); ev(24, 0, 9999)
    for k in (25, 27, 29): ev(k, 0, 0, 64); ev(k, 0, 5, 127)
    ev(30, 0, 0, 5); ev(30, 0, 3, 100)
    ev(31, P, 0, 5); ev(31, P, 0, 0)
    for b in (0, 1, 5): ev(33, P, b)
    ev(33, P, 0)
    for k in (32, 34, 35, 36, 37, 38): ev(k, P, 0); ev(k, P, 1)
    for k in (39, 40, 41): ev(k, P)
    ev(42, 0); ev(42, 3); ev(43, 0); ev(43, 2); ev(44, 0); ev(44, 1); ev(44, 2); ev(45)
    for k in (46, 63, 64, 65, 66, 71, 73, 74, 75, 76, 77, 78): ev(k, 0)
    ev(47, P, 0, 100); ev(47, P, 9999, 100)
    ev(48, 0); ev(48, 9999); ev(49, P); ev(51)
    ev(50, 0xFFFF, 100)
    ev(52, P, 0, 100); ev(52, P, 9999, 100)
    ev(53, 0); ev(53, 1); ev(53, 2)
    n = len(L)
    ev(56, P, 0, 0, 100)                  # a source started
    ev(56, P, 2, '^%d' % n, 100); ev(56, P, 3, 0, 100); ev(56, P, 1, '^%d' % n); ev(56, P, 1, 0xFFFF); ev(56, P, 2, 0xFFFF, 1)
    ev(56, P, 9, 0, 0); ev(57, P, 0, 1, 50); ev(57, P, 3, 1, 50)
    ev(58, 0); ev(58, 5); ev(58, 20); ev(62)
    ev(79, 0); ev(80, 0, 1); ev(80, 0, 1)
    ev(81); ev(200)
    ev(55, P); ev(55, P)
    CALLS = L
    AT = 2700; PER = 4
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_sound3():
    g = _p9_sound3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-soundpause'] = ('route', 'boot; freeskate 0; wait 400; pausefor 700', None, dict(_p9_env_sound3()),
              "paths9: soundEvent while the game is paused (game mode 3), every event 0 to 80, by guest call in the Movie Lot's free skate (test only)")
# p9 end cov-soundpause

# p9 begin cov-quit1
def _p9_quit1():
    # The game's own exit request (requestFrameWait 0x137D48: the main loop's flag set), by guest call in the front end (test only);
    # the run ends there.
    CALLS = [('~0x137D48',)]
    AT = 2000; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_quit1():
    g = _p9_quit1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-quit1'] = ('route', 'boot; wait 2600', None, dict(_p9_env_quit1()),
              "paths9: the game's own exit request (requestFrameWait) by guest call in the front end: the main loop ends (test only)")
# p9 end cov-quit1


# p9 begin cov-misc3
def _p9_misc3():
    # More calls no code makes, by guest call in the Movie Lot's free skate (test only): the heap probes (testHeapLimits allocates 1 KB blocks
    # until the heap or 8 MB is used up and frees them again in the same call; largestFreeBlock grows a request in 1 KB steps until the heap
    # says no, frees it), the pad list's n-th pad, the pad record's field and its alias, the texture-by-hash search (the pool of loaded
    # pictures walked with matchTextureHash), the movie code's stubs and callback slots (set to 0: their value at rest).
    CALLS = [('~0x138208', 1), ('~0x138120', 0x400, 1), ('~0x138120', 0x100, 1), ('~0x138120', 0x1000, 1),
             ('~0x14C6B0', 0), ('~0x14C6B0', 1), ('~0x14C6B0', 7), ('~0x14C6B0', 100),
             ('~0x128768', 0), ('~0x128768', 0x12345678), ('~0x1287E8',), ('~0x119650',), ('~0x11A540', 0, 0), ('~0x11A558', 0, 127)]
    AT = 2300; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_misc3():
    g = _p9_misc3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-misc3'] = ('route', 'boot; freeskate 0; wait 300', None, dict(_p9_env_misc3()),
              "paths9: heap probes, the pad list, the texture-by-hash search and the movie code's stubs, by guest call in the Movie Lot's free skate (test only)")
# p9 end cov-misc3



# p9 begin cov-material
def _p9_mat1():
    # buildMaterialLayers (0x102E00) and, through it, buildMaterialRegisters (0x103A40) on a copy of a material the renderer is about to build
    # (the hook is buildMaterialRegisters' own entry in the Movie Lot's free skate: a0 the material, 0x290 bytes copied into a heap block), the
    # copy's type (the 64-bit word at +0x40, bits 1 to 8) set to every value the jump table holds and past it, and its flags word (+0)
    # set to several bit patterns (test only): the kinds of material and the register paths no level's materials take.
    HK = 0x103A40
    items = []
    def add(t, *a): items.append((t,) + a)
    CALLS = []
    # the hook is the call's own function, so the items are given as (target, args) and written out below with the hook entry's a0
    H = '^0'
    L = ['0x%X@2400:~0x138008:0x290' % HK, '0x%X@+1:~0x136B60:%s:%%@4:0x290' % (HK, H)]
    for k in list(range(0, 13)) + [20, 255]:
        for f in (0, 0xFFFFFFFF, 0x55555555, 0xAAAAAAAA, 0x1, 0x80000000):
            L += ['0x%X@+0:W:%%(^0+0x40):%#x' % (HK, k << 1), '0x%X@+0:W:%%(^0+0):%#x' % (HK, f), '0x%X@+1:~0x102E00:^0' % HK]
    ENV = {'ZX_GUEST_CALL': ','.join(L)}
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_mat1():
    g = _p9_mat1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-material'] = ('route', 'boot; freeskate 0; wait 500', None, dict(_p9_env_mat1()),
              "paths9: buildMaterialLayers and buildMaterialRegisters on copies of a material the renderer is building, with every type and several flag patterns set (the hook is buildMaterialRegisters' entry; test only)")
# p9 end cov-material

# p9 begin cov-pause7
ROUTES['cov-pause7-01'] = ('route', 'boot; career 1; tweaks; skate 1; pausetour 7', None, dict(FRONT_END_SKIPPED),
    "paths9: the career pause menu's Level Select and Exit entries (routes.py pausetour 7, 8) [boot; career 1; tweaks; skate 1; pausetour 7; ]")
ROUTES['cov-pause7-02'] = ('route', 'boot; career 1; tweaks; skate 1; pausetour 8', None, dict(FRONT_END_SKIPPED),
    "paths9: the career pause menu's Level Select and Exit entries (routes.py pausetour 7, 8) [boot; career 1; tweaks; skate 1; pausetour 8; ]")
ROUTES['cov-pause7-03'] = ('route', 'boot; career 2; tweaks; skate 1; pausetour 7,8', None, dict(FRONT_END_SKIPPED),
    "paths9: the career pause menu's Level Select and Exit entries (routes.py pausetour 7, 8) [boot; career 2; tweaks; skate 1; pausetour 7,8; ]")
# p9 end cov-pause7

# p9 begin cov-kbd
def _p9_kbd1():
    # The zgui keyboard widget's input handler (handleGuiKeyboardInput 0x176398: (widget, {changed buttons, held buttons})) called again by guest call
    # at its own entry while the Park Editor's save dialog shows the keyboard (test only), with every button bit alone, the four directions
    # several times over (the cursor wraps round the rows), all bits, changed without held and held without changed: the keys' cursor movement, wrap-around, select, delete,
    # cancel and end paths. The game's own call follows each.
    EV = []
    for rep in range(3):
        for b in (1, 2, 4, 8, 1, 2, 4, 8):
            EV.append((b, b))
    for b in range(16):
        EV.append((1 << b, 1 << b))
    EV += [(0xFFFF, 0xFFFF), (0, 0xFFFF), (0xFFFF, 0), (0, 0), (0x8, 0x8), (0x8, 0x8), (0x80, 0x80), (0x40, 0x40)]
    H = 0x176398
    items = []
    for k, (s5, dst) in enumerate(EV):
        items.append('0x%X@%s:~0x%X:%%@4:=%#x/%#x' % (H, '1' if k == 0 else '+1', H, s5, dst))
    ENV = {'ZX_GUEST_CALL': ','.join(items)}
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_kbd1():
    g = _p9_kbd1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-kbd'] = ('route', 'boot; pe2enter 1; pe2save PE AI; pe2save ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 ZYXWVUTSRQPONMLKJIHGFEDCBA; pe2save A1-+!? B2-+!?; pe2exit', None, dict(_p9_env_kbd1()),
              "paths9: the zgui keyboard's input handler called again by guest call with every button, the directions round the rows and combinations while the Park Editor's save dialog shows it (test only)")
# p9 end cov-kbd


# p9 begin cov-libc3
def _p9_libc3():
    # libc's number parsing and formatting on the inputs the game never gives them, by guest call in the front end (test only):
    # strtod and atof (every spelling, range and failure: _strtod_r's big-number paths, 0x22B788), strtol and strtoul in every
    # base with overflow and signs, _sprintf_r (newlib's integer-only vfiprintf: every conversion, flag, width, precision and
    # length modifier) and sscanf (every conversion, a scan set, a width, assignment suppression, %n, matching failures).
    RE = '%*0x2619D4'
    CALLS = []
    for s in ('0', '-0', '1', '1.5', '123456789012345678901234567890', '.5', '5.', '1e10', '1e-10', '1e308', '1e309', '1e-324', '1e-400',
              '4.9406564584124654e-324', '2.2250738585072014e-308', '1.7976931348623157e308', '0.1', '123.456e+2', '  \t 42', '+7',
              '-inf', 'inf', 'infinity', 'nan', 'nan(0x1)', '0x1.8p3', '0x10', '1e', '1e+', 'e5', '', 'abc', '1.2.3',
              '1234567890123456789012345678901234567890.1234567890123456789012345678901234567890',
              '0.0000000000000000000000000000000000001234', '1e99999999999', '-1e-99999999999', '9007199254740993', '9007199254740992.5',
              '0.30000000000000004', '1.00000000000000011102230246251565404236316680908203125', '3.14159265358979323846', '2.5e-3x',
              '100000000000000000000000', '1E5', '1.e5', '-.5e-2', '00012.5', '0e0', '0.0e-5', '7.7e1', '5e-7', '123456789e-30'):
        CALLS.append(('~0x22C688', 's' + s, '#8'))
    for s in ('3.5', '-2e3', 'x', '1e400'):
        CALLS.append(('~0x227A60', 's' + s))
    for s, base in (('0', 0), ('0x1F', 0), ('0X1f', 16), ('077', 0), ('0b101', 0), ('-12345', 10), ('+99', 10), ('  -42abc', 10), ('zz', 36), ('ZZ', 36),
                    ('2147483647', 10), ('2147483648', 10), ('-2147483648', 10), ('-2147483649', 10), ('4294967295', 10), ('4294967296', 10),
                    ('99999999999999999999', 10), ('0x', 16), ('0x', 0), ('1', 1), ('1', 37), ('', 10), ('12', 2), ('101', 2), ('8', 8), ('777', 8), ('   ', 10),
                    ('- 5', 10), ('0x8000000', 16), ('-0x80000000', 16), ('7fffffff', 16), ('80000000', 16)):
        CALLS += [('~0x225338', 's' + s, '#8', base), ('~0x22C900', 's' + s, '#8', base), ('~0x225100', RE, 's' + s, '#8', base)]
    F = [('%d/%i/%u', 3, -5, 7), ('%o %x %X', 8, 255, 255), ('%c%c%c', 65, 66, 67), ('%s/%10s/%-10s/%.2s', 'sabc', 'sdef', 'sghi', 'sjklmn'),
         ('%5d/%-5d/%05d/%+d/% d', 42, 42, 42, 42, 42), ('%#x/%#o/%#X', 255, 8, 255), ('%.3d/%5.3d/%-8.4d/', 7, 7, 7), ('%ld %lu %lx', -1, -1, -1),
         ('%lld %llu %llx', 'L0x7FFFFFFFFFFFFFFF', 'L0xFFFFFFFFFFFFFFFF', 'L0x123456789ABCDEF0'), ('%hd %hu %hhd', 70000, 70000, 300),
         ('%p %p', 0x1234, 0), ('%%/%5%/', ), ('%*d/%-*d/%.*d', 6, 5, 6, 5, 3, 5), ('%.*s/%*s', 2, 'sabcdef', 8, 'sxy'), ('%n%d', '#8', 5),
         ('%d %d %d %d %d', 1, 2, 3, 4, 5), ('%s', 's'), ('%.0d/%.0s/', 0, 'abc'), ('%+5d/%+-5d/%+05d', 3, 3, 3), ('%x %X %o', -1, -1, -1),
         ('%u', 4294967295), ('%d', -2147483648), ('%c', 0), ('%5c/%-5c/', 65, 66), ('%#5x/%#-8o/%#08x', 255, 8, 255),
         ('%lc', 66), ('%q', 1), ('%', ), ('abc%', ), ('%1000d', 5), ('%-1000s', 'sx'), ('%.1000s', 'sy'), ('%z', 1), ('%ld%ld', 9, 9),
         ('%5.1d/% 5d/%+.2d', 9, 9, 9), ('%#.3o/%#.0x', 8, 0), ('%Lf', 1), ('%hs', 'sa'), ('%e %f %g', 0, 0, 0)]
    for f in F:
        CALLS.append(('~0x224398', RE, '#2200', 's' + f[0]) + tuple(f[1:]))
    S = [('12 34', '%d %d', '#4', '#4'), ('0x1F 077 -12', '%i %i %i', '#4', '#4', '#4'), ('ff 17 255', '%x %o %u', '#4', '#4', '#4'),
         ('abc def', '%s %s', '#16', '#16'), ('abcdef', '%3s%s', '#16', '#16'), ('x y', '%c %c', '#4', '#4'), ('xyz', '%2c', '#8'),
         ('hello,world', '%[a-z],%[a-z]', '#16', '#16'), ('abc123', '%[^0-9]%d', '#16', '#4'), ('aAbB', '%[a-zA-Z]', '#16'), (']x', '%[]x]', '#16'),
         ('a-b', '%[a\\-b]', '#16'), ('12 34', '%*d %d', '#4'), ('12 34', '%*s %s', '#16'), ('  42', '%d%n', '#4', '#4'), ('7', '%hd', '#4'),
         ('70000', '%hd', '#4'), ('123456789012', '%ld', '#8'), ('123456789012345', '%lld', '#8'), ('-5', '%u', '#4'), ('99999999999', '%d', '#4'),
         ('12abc', '%d%s', '#4', '#16'), ('abc', '%d', '#4'), ('', '%d', '#4'), ('1', '%d %d', '#4', '#4'), ('50%', '%d%%', '#4'), ('50x', '%d%%', '#4'),
         ('1.5 2.5e3 -7', '%f %e %g', '#4', '#4', '#4'), ('1.5', '%lf', '#8'), ('0x1A', '%x', '#4'), ('0x', '%x', '#4'), ('-0x1A', '%i', '#4'),
         ('12345678', '%4d%4d', '#4', '#4'), ('12345678', '%3hd', '#4'), ('abc', '%1c%1c', '#4', '#4'), ('1234', '%p', '#4'),
         ('a b c d e f', '%s %s %s %s %s', '#16', '#16', '#16', '#16', '#16'), ('9', '%1d', '#4'), ('+5 -6', '%d %d', '#4', '#4'),
         ('0777 0xff 099', '%o %x %o', '#4', '#4', '#4'), ('  \n\t 5', '%d', '#4'), ('5 6', '%d%d%d', '#4', '#4', '#4'),
         ('abc', '%[', '#16'), ('abc', '%[a', '#16'), ('abc', '%[^', '#16'), ('abc', '%z', '#4'), ('5', '%ld %lu', '#4', '#4'), ('5', '%5', '#4')]
    for text, fmt, *outs in S:
        CALLS.append(('~0x224478', 's' + text, 's' + fmt) + tuple(outs))
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_libc3():
    g = _p9_libc3()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-libc3'] = ('route', 'boot; wait 3400', None, dict(_p9_env_libc3()),
              'paths9: libc strtod, atof, strtol, strtoul, _sprintf_r and sscanf on every spelling, flag, width, base and failure, by guest call in the front end (test only)')
# p9 end cov-libc3

# p9 begin cov-animstream
def _p9_stream1():
    # The animation stream reader (openZfileLibrary 0x1DEB38 and its record's functions: count, frames, a frame found, the header and data of the current
    # frame read, the cursor advanced, closed) on ASSETS/OVERLAY.ZAL (the one disc file with the stream header, 0x2A81511C), by guest call in the front end
    # (test only); a path that is no stream and one that does not exist
    CALLS = [('~0x138008', 0x100), ('~0x138008', 0x40000), ('~0x2249B4', '^0', 'sassets\\overlay.zal'),
             ('~0x1DEB38', '^0'),                                                    # 3: the record
             ('~0x1DED78', '^3'), ('~0x1DED80', '^3'), ('~0x1DEE60', '^3')]
    for k in range(4):
        CALLS += [('~0x1DEE68', '^3'), ('~0x1DEF30', '^3', '^1'), ('~0x1DF148', '^3')]
    CALLS += [('~0x1DEDE8', '^3', 0, 0), ('~0x1DEDE8', '^3', 5, 1), ('~0x1DED18', '^3'),
              ('~0x2249B4', '^0', 'sassets\\uilib.zal'), ('~0x1DEB38', '^0'),
              ('~0x2249B4', '^0', 'sassets\\nosuch.zal'), ('~0x1DEB38', '^0'), ('~0x1DED18', 0)]
    AT = 1700; PER = 1
    return {k: v for k, v in locals().items() if k in ('CALLS', 'ENV', 'AT', 'PER', 'GAP', 'HOOK')}


def _p9_env_stream1():
    g = _p9_stream1()
    env = g['ENV'] if 'ENV' in g else _guest_script(g['CALLS'], [], at=g.get('AT', 1500), per=g.get('PER', 8),
                                                    hook=g.get('HOOK', 0x100968), gap=g.get('GAP', 1))
    return env


ROUTES['cov-animstream'] = ('route', 'boot; wait 3000', None, dict(_p9_env_stream1()),
              'paths9: the animation stream reader (open, count, frames, a frame found, header and data read, cursor advanced, closed) on ASSETS/OVERLAY.ZAL, a file that is no stream and a missing one, by guest call in the front end (test only)')
# p9 end cov-animstream

# p9 begin cov-menumash
ROUTES['cov-menumash-01'] = ('route', 'boot; menumash 600 1', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 600 1; ]')
ROUTES['cov-menumash-02'] = ('route', 'boot; menumash 600 2', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 600 2; ZX_PAD_PORT1=mirror]')
ROUTES['cov-iop-files'] = ('route', 'boot; career 1', None, dict(ZX_DISC='ps2', ZX_MOVIES='hle'),
    'zgehand18: ZCDDVD and ZAUDIO over the IOP, the overrides off: the disc client, the sound RPC, the movie start and end [boot; career 1; ZX_DISC=ps2 ZX_MOVIES=hle]')
ROUTES['cov-menumash-03'] = ('route', 'boot; menumash 300 3', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 3; ]')
ROUTES['cov-menumash-04'] = ('route', 'boot; menumash 300 4', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 4; ZX_PAD_PORT1=mirror]')
ROUTES['cov-menumash-05'] = ('route', 'boot; menumash 300 5', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 5; ]')
ROUTES['cov-menumash-06'] = ('route', 'boot; menumash 300 6', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 6; ZX_PAD_PORT1=mirror]')
ROUTES['cov-menumash-07'] = ('route', 'boot; menumash 300 7', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 7; ]')
ROUTES['cov-menumash-08'] = ('route', 'boot; menumash 300 8', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 8; ZX_PAD_PORT1=mirror]')
ROUTES['cov-menumash-09'] = ('route', 'boot; menumash 300 9', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 9; ]')
ROUTES['cov-menumash-10'] = ('route', 'boot; menumash 300 10', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 10; ZX_PAD_PORT1=mirror]')
ROUTES['cov-menumash-11'] = ('route', 'boot; menumash 300 11', None, dict(),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 11; ]')
ROUTES['cov-menumash-12'] = ('route', 'boot; menumash 300 12', None, dict(ZX_PAD_PORT1='mirror'),
    'paths9: random seeded button presses from the main menu (routes.py menumash) [boot; menumash 300 12; ZX_PAD_PORT1=mirror]')
# p9 end cov-menumash

# zgui script lines run one at a time by its command dispatcher (FUN_001820F8, the line's first word looked up
# in the command table 0x245448), by guest call in the front end (test only): variables of each type declared
# under Root (DECLARE: Uint8 Bool Sint32 ZFloat ZRGBA_PAL Z3DVector ZQuaternion String Alignment ZAssetID Event,
# the type names of 0x245F70..), set from each other and from literals, LINEAR, ADD/SUB/MULT/DIV, STRCAT,
# IF: the types' conversions no AI menu script asks for; then the commands only DM2's, BMX XXX's or AI's
# leftover menu scripts use (FORMAT, STREXTRACT, STRTOKEN, REPORTLIB, ROTQUATY; also STRCMP, USE_STR,
# ROTQUATX/Z), LOADIMAGE of the disc's BMPs, LOADLIB of AI's leftover MENU/*.ZIF (ANIMS.ZIF has the UNAD
# chunk), the variables undeclared.
_ZT = (('Uint8', 'U8'), ('Bool', 'B'), ('Sint32', 'S'), ('ZFloat', 'F'), ('ZRGBA_PAL', 'P'), ('Z3DVector', 'V'),
       ('ZQuaternion', 'Q'), ('String', 'T'), ('Alignment', 'A'), ('ZAssetID', 'I'), ('Event', 'E'))
_ZL = ['DECLARE %s Root.P7%s' % _t for _t in _ZT] + ['SET Root.P7F 1.5', 'SET Root.P7T "2.5"']
_ZL += ['SET Root.P7%s Root.P7%s' % (_a[1], _b[1]) for _a in _ZT[:8] for _b in _ZT[:8] if _a != _b]
for _t, _n in _ZT:
    _ZL += ['SET Root.P7%s %s' % (_n, _l) for _l in ('{1.5}', '{7}', '{1 2 3}', '{10 20 30 40}', '{abc}', '{0.5 0.5 0.5 0.5}')]
    _ZL += ['LINEAR Root.P7%s {0} {255} Root.P7F' % _n, 'LINEAR Root.P7%s {0 0 0} {9 9 9} Root.P7F' % _n,
            'ADD Root.P7%s Root.P7F' % _n, 'MULT Root.P7F Root.P7%s' % _n, 'SUB Root.P7%s {1}' % _n,
            'DIV Root.P7%s {2}' % _n, 'STRCAT Root.P7T Root.P7%s' % _n]
_ZL += ['SET Root.P7T {12 34}', 'FORMAT Root.P7T {v %d %f %s} Root.P7S Root.P7F Root.P7T', 'FORMAT Root.P7T Root.P7S',
        'STREXTRACT Root.P7T Root.P7T', 'STREXTRACT Root.P7T Root.P7T {1} {2}', 'SET Root.P7T {a,b,c}',
        'STRTOKEN Root.P7T Root.P7T Root.P7S', 'STRTOKEN Root.P7T {a b c} {1}', 'REPORTLIB', 'REPORTLIB Root.P7T',
        'USE_STR Root.P7T', 'STRCMP Root.P7T {a} Root.P7S', 'STRCMP Root.P7T Root.P7T Root.P7S',
        'ROTQUATX Root.P7Q {30}', 'ROTQUATY Root.P7Q {30}', 'ROTQUATZ Root.P7Q {30}', 'ROTQUATY Root.P7Q Root.P7F',
        'IF Root.P7F > {1}', 'DEBUG_PRINT Root.P7T', 'LINEAR Root.P7V Root.P7V Root.P7V Root.P7F',
        'LOADIMAGE P7Img assets\\menu\\gapstr~1.bmp', 'LOADIMAGE P7Img2 assets\\loading\\movie\\origin~1\\movie0.bmp P7Done',
        'LOADLIB P7Anims assets\\menu\\anims.zif', 'LOADLIB P7Common assets\\menu\\common.zif',
        'LOADLIB P7Levels assets\\menu\\levels.zif LoadComplete', 'LOADLIB P7Players assets\\menu\\players.zif']
_ZL += ['UNDECLARE Root.P7%s' % _t[1] for _t in _ZT]
ROUTES['cov-zguivars'] = ('route', 'boot; wait 1700; career 1; skate 1', None,
                          _guest_script([(0x1820F8, 's' + _l) for _l in _ZL], []),
                          'paths7: zgui script lines by guest call in the front end (test only): every variable type\'s '
                          'conversions, the commands only other scripts use, the leftover menu libraries, then the '
                          'Movie Lot')
# The menu natives no shipped menu script CALLs (inlineMenuVibrateController; the inlinesound UI commands,
# flagged script-dead), run by the zgui CALL command through the dispatcher (as cov-zguivars) in the front end.
ROUTES['cov-zguicalls'] = ('route', 'boot; wait 1700; career 1; skate 1', None,
                           _guest_script([(0x1820F8, 'sCALL ' + _n) for _n in
                                          ('inlineMenuVibrateController', 'inlinesoundUIUpdate', 'inlinesoundChangedTrack',
                                           'inlinesoundChangedSoundtrack')], [], per=1),
                           'paths7: menu natives no menu script calls, by zgui CALL lines (test only: guest calls), '
                           'then the Movie Lot')
# libc's sscanf (0x224478, newlib: AI parses only integers and short words with it) on what the game never
# hands it: floats and doubles (strtod and its big-number helpers), a scan set, unsigned longs past their
# range, %i bases, a failed match (ungetc), by guest call in the front end (test only).
_SCANF = (('3.25 -1e5 1234567890123', '%f %lf %lld'), ('abc123def', '%[a-c]%lu%2s'), ('0x1F 077 -12', '%i %i %i'),
          ('12345678901234567890', '%lu'), ('1.7976931348623157e308 4.9e-324 0.1', '%lf %lf %lf'),
          ('  42rest', '%d%n'), ('x', '%d'), ('123456789012345678901234567890.5e-10 2.5E+3', '%lf %e'),
          ('9007199254740993 18446744073709551615', '%lf %llu'), ('[x] yes', '[%[^]]] %3c'))
ROUTES['cov-libscanf'] = ('route', 'boot; wait 1700; career 1; skate 1', None,
                          _guest_script([(0x224478, 's' + _t, 's' + _f, '#16', '#16', '#16', '#16')
                                         for _t, _f in _SCANF], [], per=1),
                          'paths7: libc\'s sscanf on floats, doubles, scan sets and long numbers (test only: guest '
                          'calls in the front end), then the Movie Lot')
# zgui's LOADMODEL (only DM2's menus use it: `LOADMODEL *LoadingPlayer *CurLib LoadComplete`) of AI's own
# model files ASSETS/MISC/PIGEON.ZIT and RAT.ZIT, with and without a completion, then LOADZSC (no script uses
# it) of AI's leftover MENU/MAIN.ZSC, the front end left running (test only: guest calls).
ROUTES['cov-zguimodel'] = ('route', 'boot; wait 2400', None,
                           _guest_script([(0x1820F8, 'sLOADMODEL P7Mdl assets\\misc\\pigeon.zit P7Done'),
                                          (0x1820F8, 'sLOADMODEL P7Mdl2 assets\\misc\\rat.zit'),
                                          (0x1820F8, 'sLOADZSC assets\\menu\\main.zsc')], [], per=1),
                           'paths7: zgui LOADMODEL of AI\'s own models and LOADZSC of its leftover main menu script '
                           '(test only: guest calls in the front end)')


# The game situation each route plays (round corpusfast; the career
# challenges from Wilikai's FAQ v0.91 and Tom Hayes' GameFAQs walkthrough):
# the corpus' pruning (fncheck_corpus.py prune-plan) keeps, per function, at
# least one recording of every situation it was recorded in. Routes of one
# situation are the same play again (another seed, the cov- re-recordings of
# new paths); the challenges a route plays to its end are named.
LEVELS = ('Tutorial', 'Movie Lot', 'Civic Center', 'Industrial', 'Boardwalk', 'Cannery', 'Airfield', 'Museum')
ROUTES['play-movielot'] = ('input', 'play-movielot', 33163, {}, 'a played session: the movielot career played by tools/playbot/ai.py')
ROUTES['play-civic'] = ('input', 'play-civic', 21030, {}, 'a played session: the civic career played by tools/playbot/ai.py')
ROUTES['play-industrial'] = ('input', 'play-industrial', 21220, {}, 'a played session: the industrial career played by tools/playbot/ai.py')
ROUTES['play-boardwalk'] = ('input', 'play-boardwalk', 20367, {}, 'a played session: the boardwalk career played by tools/playbot/ai.py')
ROUTES['play-cannery'] = ('input', 'play-cannery', 20839, {}, 'a played session: the cannery career played by tools/playbot/ai.py')
ROUTES['play-airfield'] = ('input', 'play-airfield', 21449, {}, 'a played session: the airfield career played by tools/playbot/ai.py')
ROUTES['play-museum'] = ('input', 'play-museum', 20295, {}, 'a played session: the museum career played by tools/playbot/ai.py')
ROUTES['play-tutorial'] = ('input', 'play-tutorial', 14695, {}, 'a played session: the tutorial career played by tools/playbot/ai.py')

ROUTES['play2-movielot'] = ('input', 'play2-movielot', 13122, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-civic'] = ('input', 'play2-civic', 27817, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-industrial'] = ('input', 'play2-industrial', 27816, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-boardwalk'] = ('input', 'play2-boardwalk', 27907, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-cannery'] = ('input', 'play2-cannery', 28612, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-airfield'] = ('input', 'play2-airfield', 28381, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-museum'] = ('input', 'play2-museum', 28124, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play2-career'] = ('input', 'play2-career', 119703, {}, 'a played career on a fresh card by tools/playbot/ai.py: Movie Lot to 8 points, Civic opened and played')
ROUTES['play2-tutorial'] = ('input', 'play2-tutorial', 16582, {}, 'a played session of tools/playbot/ai.py')
ROUTES['play3-goals'] = ('input', 'play3-goals', 20623, {}, 'a played career on a fresh card by tools/playbot/ai.py (play goals): Movie Lot to 8 points, left and entered again twice (saveCareerLevel three times), SELECT objectives page each visit, held wall rides')
ROUTES['play3-statue'] = ('input', 'play3-statue', 9804, {}, "a played session by tools/playbot/ai.py (Civic, dev unlock): the Broken Statue's giver, a wall ride on the statue's own box (WallRideStart, the audio sync, syncStart: the objective complete, the truck dance)")
ROUTES['play3-rescue'] = ('input', 'play3-rescue', 9398, {}, "a played split screen by tools/playbot/ai.py: the menus by pad script (port 1 mirrors port 0), the Multiplayer menu's Animal Rescue, animals picked up (pickUpAnimal, a crash's dropAnimals) to the round's end and its results")
ROUTES['play3-career'] = ('input', 'play3-career', 155760, {}, "a played career on a fresh card by tools/playbot/ai.py: Movie Lot to 8 points, Civic to 8 (the Broken Statue's wall ride among them), the third level (Industrial) opened and played to 2 points")
ROUTES['play3-double'] = ('input', 'play3-double', 22071, {}, "a played session by tools/playbot/ai.py (Boardwalk, dev unlock): lines, and double rails (the wharf's grind rail) ridden as Double Grinds (UP three times, the grind button: 7 of them)")
ROUTES['play3-vaults'] = ('input', 'play3-vaults', 22548, {}, "a played session by tools/playbot/ai.py (Movie Lot, dev unlock, PLAY_VARIETY): lines, the level's objectives (5 challenge points), vaults (4), a skitch, rails")
ROUTES['play3-park'] = ('input', 'play3-park', 15222, {}, 'a played Park Editor by tools/playbot/ai.py on a fresh card: a new park, four towers (terrain raised or lowered, a piece on it, the ground moved under it, the piece deleted and placed again), a Test Run skated without a plan (ollies, grabs, manuals), back to the editor')
SITUATIONS = {
    'lc': ('Tutorial', 'front end, the Tutorial career and its goals (the logiccheck input)', ['the Tutorial\'s goals']),
    'cov-lc': ('Tutorial', 'front end, the Tutorial career and its goals (the logiccheck input)', ['the Tutorial\'s goals']),
    'career': ('Movie Lot', 'the Movie Lot career, the pause menu, a save (the careercheck input)', []),
    'cov-career': ('Movie Lot', 'the Movie Lot career, the pause menu, a save (the careercheck input)', []),
    'moves': ('Tutorial', 'the Tutorial\'s lessons: stall, poles, skitch, wall ride and jump, vault, grind, manual, '
              'cess slide', ['the Tutorial lessons (moves)']),
    'grinds': ('Tutorial', 'grind tricks combined on the Tutorial rail', []),
    'tricks-b': ('Tutorial', 'grind tricks combined on the Tutorial rail', []),
    'tricks-c': ('Tutorial', 'grind tricks combined on the Tutorial rail', []),
    'cov-combos': ('Tutorial', 'the grind combos asked for on the rail (SF, FM, MS)', []),
    'grindx': ('Tutorial', 'the Tutorial grind entered, jumped off and poked (enter paths, exits, speeds)', []),
    'cov-grinds': ('Tutorial', 'the Tutorial grind entered, jumped off and poked (enter paths, exits, speeds)', []),
    'cov-grindpoke': ('Tutorial', 'the Tutorial grind entered, jumped off and poked (enter paths, exits, speeds)', []),
    'tricks-a': ('Tutorial', 'hand plants, the stall table, skitches', []),
    'cov-stalls': ('Tutorial', 'hand plants, the stall table, skitches', []),
    'cov-holds-a': ('Tutorial', 'hand plants and grinds held and jumped off early and late, pole aims', []),
    'cov-poles': ('Tutorial', 'the swing poles aimed and left', []),
    'cov-pickups': ('Tutorial', 'the Power Ups, Attributes and Juice lessons (pickups)', []),
    'cov-controls': ('Tutorial', 'the analog stick in and past the dead zone', []),
    'gs': ('Tutorial', 'the Tutorial skated with the GS on (the CPU renderer)', []),
    'splitscreen': ('Movie Lot', 'MULTIPLAYER split screen, Most Points', []),
    'cov-splitscreen': ('Movie Lot', 'MULTIPLAYER split screen, Most Points', []),
    'movielot': ('Movie Lot', 'the Movie Lot career skated, taxis hit, Kelly\'s challenge completed',
                 ['Movie Lot: 40,000 Point Timed Run (Kelly), completed']),
    'careers-a': ('Movie Lot', 'Kelly\'s challenge failed, a score rollover, a level up',
                  ['Movie Lot: 40,000 Point Timed Run (Kelly), failed']),
    'cov-traffic': ('Movie Lot', 'the Movie Lot taxis hit at two speeds', []),
    'cov-npcs-1': ('Movie Lot', 'the Movie Lot career\'s NPCs run into', []),
    'cov-npcs-4': ('Boardwalk', 'the Boardwalk career\'s NPCs run into', []),
    'spines': ('Movie Lot', 'free skate: the Air Spine both ways, spin buttons held', []),
    'cov-spineturn': ('Movie Lot', 'free skate: the Air Spine both ways, spin buttons held', []),
    'museum': ('Museum', 'the Museum career: a double grind, skated', []),
    'doublegrind': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'cov-doublegrind': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'cov-doublegrind2': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'cov-holds-b': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'cov-dgenter': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'cov-dgenter2': ('Museum', 'the Museum Double Grinds and the Hand Grind (entered, held, jumped off, let go)', []),
    'careers-b': ('Civic Center, Industrial', 'the Civic Center and Industrial careers skated', []),
    'careers-c': ('Boardwalk, Cannery, Airfield', 'the Boardwalk, Cannery and Airfield careers skated', []),
    'cov-hazards': ('Movie Lot to Museum', 'the careers\' water, fire and out-of-bounds faces', []),
    'cov-sound': ('Movie Lot', 'the sound options: front end, pause, split screen', []),
    'cov-sound-play': ('Tutorial to Museum', 'play in every career level (the sound events)', []),
    'cov-mpresults': ('Movie Lot', 'MULTIPLAYER split screen: every game to its results', []),
    'cov-zones-a': ('Movie Lot to Boardwalk', 'the careers\' trigger zones touched, play, a level entered again', []),
    'cov-zones-b': ('Cannery to Museum', 'the careers\' trigger zones touched, play, a level entered again', []),
    'cov-quit': ('Movie Lot', 'the game\'s exit (shutdown) from a level', []),
    'cov-quit-fe': ('front end', 'the game\'s exit (shutdown) from the main menu', []),
    **{'cov-emitters-%d' % _k: (('Movie Lot', 'Civic Center', 'Industrial', 'Boardwalk', 'Cannery', 'Airfield',
                                 'Museum')[_k - 1], 'the level\'s emitters on and an explosion (test only), skated', [])
       for _k in (1, 2, 4, 5, 6, 7)},
    'cov-splash': ('Boardwalk', 'a splash effect at the skater (test only), skated', []),
    'cov-filewatch': ('Movie Lot', 'zfile\'s watcher on (test only), levels loaded', []),
    'cov-ragdoll': ('Airfield', 'the players\' ragdoll stepped (test only), skated', []),
    'cov-fxclasses': ('Movie Lot', 'the lens flare and the emitter-only effect classes (test only), skated', []),
    'cov-cdvd': ('Movie Lot', 'files read through the game\'s CD client (ZX_DISC=ps2), levels loaded', []),
    'cov-replay': ('Movie Lot', 'free skate with the input recorder started, stopped and played back (test only)', []),
    'cov-replay2': ('Movie Lot', 'free skate, the input recorder started and played back again and again (test only)', []),
    'cov-pieces': ('Airfield, Tutorial', 'Place and Trick: the Vacuum Tool, the pieces ridden; the Tutorial left', []),
    'cov-pausepages': ('Movie Lot, Airfield', 'the career pause pages: fly-thrus, the trick list\'s Specials', []),
}
for _k in range(2, 9):
    SITUATIONS['levels-%d' % _k] = ('free skate list entry %d' % _k, 'free skate entry %d, skated' % _k, [])
for _k, _name in enumerate(LEVELS):
    for _r in ('mash-%d', 'cov-mash-%d', 'cov-mash2-%d'):
        SITUATIONS[_r % _k] = (_name, 'the %s career, seeded play (mash)' % _name, [])
for _r in ('mash-zoo', 'cov-mash-zoo', 'cov-mash2-zoo'):
    SITUATIONS[_r] = ('Zoo', 'the Zoo in free skate, seeded play (mash)', [])
for _r in ('mash-estate', 'cov-mash2-estate'):
    SITUATIONS[_r] = ('Estate', 'Estate in free skate, seeded play (mash)', [])


def situation(route):
    """The situation a route plays (its own name for a route not in SITUATIONS)."""
    s = SITUATIONS.get(route.split('/')[-1])
    return '%s: %s' % (s[0], s[1]) if s else route


# Calls recorded a vsync with --new-blocks (kept: only those with new blocks).
NB_PER_VSYNC = 4
NB_PER_VSYNC_HEAVY = 1
NB_MAX = 400
LIGHT = {'ZX_FNREC_MAX': '1000', 'ZX_FNREC_PER_VSYNC': '8'}
HEAVY = {'ZX_FNREC_MAX': '100', 'ZX_FNREC_PER_VSYNC': '1'}
# One pass (round 14): each function's caps in the list file.
CAPS_LIGHT = (200, 8)
CAPS_HEAVY = (50, 1)
HEAVY_AVERAGE = 8.0
HEAVY_SITES = 10
# Functions one call of which makes this many calls or more (the level
# set-up's nearest-edge search: 100,000 and more, minutes of RAM diffs each)
# are recorded on these routes only.
DEEP_CALLS = 5000
DEEP_ROUTES = ('lc',)
# Known deep functions, for a store with no recordings yet (the level set-up's
# nearest-edge search).
DEEP_KNOWN = (0x186BF8,)


def recorder_version():
    text = open(os.path.join(ztree.GAME, 'ee', 'fncheck.h')).read()
    # The corpus's version (fncheck.h kCorpusVersion: file versions with the
    # same records share it), else the file version (older checkouts).
    m = re.search(r'constexpr uint32_t kCorpusVersion = (\d+);', text) or \
        re.search(r'struct FileHeader\s*\{.*?uint32_t version = (\d+);', text, re.S)
    if not m:
        sys.exit('readable_corpus.py: no FileHeader version in fncheck.h')
    return int(m.group(1))


def registered(build):
    """[(address, name, status)] from zx-fncheck --list of the build."""
    p = subprocess.run([os.path.join(build, 'runtime', 'zx-fncheck'), '--list'], capture_output=True, text=True)
    out = []
    for line in p.stdout.split('\n'):
        m = re.match(r'^0x([0-9a-f]+)\s+(\w+)\s+(\S+)', line)
        if m:
            out.append((int(m.group(1), 16), m.group(3), m.group(2)))
    if not out:
        sys.exit('readable_corpus.py: zx-fncheck --list gave no functions (%s)' % build)
    return out


_GENERATED = {}


def call_sites(address):
    gen = os.environ.get('ZX_RUNNER_GENERATED_DIR') or os.path.join(WORKSPACE, 'recomp', 'generated')
    if gen not in _GENERATED:
        files = {}
        for name in os.listdir(gen) if os.path.isdir(gen) else []:
            m = re.search(r'_0x([0-9a-f]+)\.cpp$', name)
            if m:
                files.setdefault(int(m.group(1), 16), name)
        _GENERATED[gen] = files
    name = _GENERATED[gen].get(address)
    if name:
        text = open(os.path.join(gen, name), errors='replace').read()
        return len(re.findall(r'// 0x[0-9a-f]+: 0x[0-9a-f]+\s+jalr?\s', text))
    return 0


def deep_functions(store_root):
    """Functions with a recorded call that made DEEP_CALLS calls or more."""
    deep = set(DEEP_KNOWN)
    for root, _, files in os.walk(store_root):
        for name in files:
            if name.endswith('.fnrec'):
                r = fnrec_index.read_file(os.path.join(root, name))
                if r.get('address') is not None and r['max_callee_calls'] >= DEEP_CALLS:
                    deep.add(r['address'])
    return deep


def averages(store_root):
    """Average callee calls per kept call, per function, from every recording in the store."""
    totals = {}
    for root, _, files in os.walk(store_root):
        for name in files:
            if name.endswith('.fnrec'):
                r = fnrec_index.read_file(os.path.join(root, name))
                if r.get('address') is None or not r['calls']:
                    continue
                c, k = totals.get(r['address'], (0, 0))
                totals[r['address']] = (c + r['callee_calls'], k + r['calls'])
    return {a: c / k for a, (c, k) in totals.items() if k}


def parse_caps(spec):
    """{address: (max, per vsync)} of --caps (ADDR:MAX[:PER_VSYNC], comma
    separated or a line each in @file); a missing per-vsync cap is the light
    one's."""
    if not spec:
        return {}
    text = open(spec[1:]).read() if spec.startswith('@') else spec
    out = {}
    for item in re.split(r'[,\n]', text):
        item = item.split('#')[0].strip()
        if not item:
            continue
        parts = item.split(':')
        out[int(parts[0], 16)] = (int(parts[1]), int(parts[2]) if len(parts) > 2 else CAPS_LIGHT[1])
    return out


def split_heavy(functions, store_root):
    avg = averages(store_root)
    light, heavy = [], []
    for address, name, status in functions:
        a = avg.get(address)
        is_heavy = (a >= HEAVY_AVERAGE) if a is not None else (call_sites(address) >= HEAVY_SITES)
        (heavy if is_heavy else light).append(address)
    return light, heavy


def rss_mb(pid):
    """The process's resident memory in MB (ps), None once it is gone."""
    try:
        out = subprocess.run(['ps', '-o', 'rss=', '-p', str(pid)], capture_output=True, text=True).stdout.strip()
        return int(out) // 1024 if out else None
    except (OSError, ValueError):
        return None


class Watch(threading.Thread):
    """Watches a runner: its peak resident memory, and stops it (by PID) at
    the pass's time limit or above --max-rss-mb (a recording grows for the
    whole run: the corpus runs are short segments)."""

    def __init__(self, pid, max_secs, max_rss):
        super().__init__(daemon=True)
        self.pid, self.max_secs, self.max_rss = pid, max_secs, max_rss
        self.peak, self.why, self.done = 0, None, threading.Event()

    def run(self):
        t0 = time.time()
        while not self.done.is_set():
            mb = rss_mb(self.pid)
            if mb is None:
                return
            self.peak = max(self.peak, mb)
            if mb > self.max_rss or time.time() - t0 >= self.max_secs:
                self.why = ('memory %d MB' % mb) if mb > self.max_rss else 'the time limit (%d s)' % self.max_secs
                try:
                    os.kill(self.pid, signal.SIGTERM)
                    time.sleep(3)
                    os.kill(self.pid, signal.SIGKILL)
                except OSError:
                    pass
                return
            self.done.wait(2.0)


class Run:
    """One pass of one route: a runner with a time limit, killed by PID."""

    def __init__(self, args, route, pass_name, addresses, folder, log, heavy=()):
        self.args, self.route, self.pass_name, self.addresses, self.folder, self.log = \
            args, route, pass_name, addresses, folder, log
        self.heavy = set(heavy)

    def env(self):
        kind, what, stop, extra, _ = ROUTES[self.route]
        env = dict(os.environ)
        # The function list as a file (ZX_FNREC=@file): a sweep's thousands
        # of addresses, each with its caps in the one-pass mode.
        listing = os.path.join(self.work, 'functions.txt')
        with open(listing, 'w') as f:
            for a in self.addresses:
                if self.args.new_blocks:
                    per = self.args.nb_per_vsync or (NB_PER_VSYNC_HEAVY if a in self.heavy else NB_PER_VSYNC)
                    f.write('0x%08x %d %d\n' % (a, NB_MAX, per))
                elif self.pass_name == 'all':
                    caps = (self.args.caps_of or {}).get(a) or (CAPS_HEAVY if a in self.heavy else CAPS_LIGHT)
                    f.write('0x%08x %d %d\n' % ((a,) + tuple(caps)))
                else:
                    f.write('0x%08x\n' % a)
        env.update({'ZX_UNPACED': '1', 'ZX_GS_NULL': '1', 'ZX_VU1_SKIP': '1', 'ZX_SAVES': 'card',
                    'ZX_READABLE': 'off', 'ZX_FNREC_ATOMIC': '1', 'ZX_FNREC_DIR': self.folder,
                    'ZX_FNREC': '@' + listing, 'ZX_DEV_UNLOCK_ALL': '1',
                    'ZX_DEBUG_UI': '0'})
        env.update(LIGHT if self.pass_name in ('light', 'all') else HEAVY)
        if self.args.new_blocks:
            env.update({'ZX_FNREC_NEW_BLOCKS': '2' if self.args.every_call else '1',
                        'ZX_FNREC_COVERED': os.path.abspath(self.args.new_blocks)})
        if self.args.mash_coverage:
            env['MASH_COVERAGE'] = os.path.abspath(self.args.mash_coverage)
        for k, v in extra.items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = v
        return env

    def pad_run(self, env, runner, work, name, stop):
        """As tools/run-game.sh: a scratch HOME, card and cwd; stops at vsync
        `stop` ([state] / [frame:upload] ticks; [run:tick] counts something
        else) or at the time limit; killed by PID."""
        for sub in ('home', 'cwd', 'app', 'shots'):
            os.makedirs(os.path.join(work, sub), exist_ok=True)
        card = os.path.join(work, 'mc0')
        if not os.path.isdir(card):
            shutil.copytree(os.path.join(WORKSPACE, 'recomp', 'run', 'mc0'), card)
        env.update({'HOME': os.path.join(work, 'home'), 'ZX_APP_SUPPORT_DIR': os.path.join(work, 'app'),
                    'ZX_APP_LOG_DIR': os.path.join(work, 'app'), 'ZX_SCREENSHOT_DIR': os.path.join(work, 'shots'),
                    'ZX_MC_ROOT': card, 'ZX_GS_BACKEND': env.get('ZX_GS_BACKEND', 'cpu')})
        logfile = os.path.join(work, name + '.log')
        with open(logfile, 'wb') as log:
            p = subprocess.Popen([runner, os.path.join(WORKSPACE, 'recomp', 'disc-root', 'SLUS_203.27')], env=env,
                                 cwd=os.path.join(work, 'cwd'), stdout=log, stderr=subprocess.STDOUT)
        t0 = time.time()
        self.watch = Watch(p.pid, self.args.max_secs, self.args.max_rss_mb)
        self.watch.start()
        why = 'time limit'
        tick_re = re.compile(rb'^\[(?:state|frame:upload)\][^\n]*?\btick=(\d+)', re.M)
        while p.poll() is None:
            if time.time() - t0 >= self.args.max_secs:
                break
            try:
                with open(logfile, 'rb') as f:
                    f.seek(max(0, os.path.getsize(logfile) - 65536))
                    ticks = tick_re.findall(f.read())
                if ticks and int(ticks[-1]) >= stop:
                    why = 'vsync %d' % int(ticks[-1])
                    break
            except OSError:
                pass
            time.sleep(1.0)
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(5)
            except subprocess.TimeoutExpired:
                p.kill()
                p.wait()
            return 'stopped at %s after %d s' % (why, time.time() - t0), logfile
        return 'exited %s after %d s' % (p.returncode, time.time() - t0), logfile

    def go(self):
        kind, what, stop, extra, _ = ROUTES[self.route]
        runner = os.path.join(self.args.build, 'runtime', 'zx-runner')
        name = 'corpus-%s-%s%s' % (self.route, self.pass_name, '-only' if self.args.only else '')
        work = os.path.join(self.args.store, 'work', name)
        shutil.rmtree(work, ignore_errors=True)
        os.makedirs(work)
        os.makedirs(self.folder, exist_ok=True)
        self.work = work
        env = self.env()
        t0 = time.time()
        if kind == 'input':
            # A played session (tools/playbot/ai.py): ab_inputs/<what>.txt, its #env lines and its pad script.
            script = ''
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ab_inputs', what + '.txt')) as f:
                for line in f:
                    if line.startswith('#env '):
                        key, _, value = line[5:].strip().partition('=')
                        env[key] = value
                    elif not line.startswith('#'):
                        script += line.strip()
            # By file: a long session's script is past the environment's limit (E2BIG at exec).
            os.makedirs(work, exist_ok=True)
            path = os.path.join(work, 'script.txt')
            with open(path, 'w') as f:
                f.write(script)
            env['ZX_PAD_SCRIPT'] = '@' + path
            env['ZX_PAD_SCRIPT_CLOCK'] = 'vsync'
            env.setdefault('ZX_STATE_LOG', '0x276FE4')
            result, logfile = self.pad_run(env, runner, work, name, stop)
        elif kind == 'pad':
            env['ZX_PAD_SCRIPT'] = subprocess.run([sys.executable, os.path.join(WORKSPACE, 'tools', what + '.py'), 'script'],
                                                    capture_output=True, text=True, cwd=WORKSPACE).stdout.strip()
            env['ZX_PAD_SCRIPT_CLOCK'] = 'vsync'
            env.setdefault('ZX_STATE_LOG', '0x276FE4')
            result, logfile = self.pad_run(env, runner, work, name, stop)
        else:
            env['RUNNER'] = runner
            env['ROUTES_DIR'] = os.path.join(work, 'routes')
            env['PYTHONPATH'] = os.path.join(WORKSPACE, 'tools')
            routes = self.args.routes_py
            # Recording slows the runner down: routes.py's wall-clock limits (a
            # batch's reply, the steering loops) are made longer.
            env.setdefault('ROUTES_SLOW', '8')
            kv = ['%s=%s' % (k, env[k]) for k in env if k.startswith('ZX_')]
            subprocess.run([sys.executable, routes, 'start', name] + kv, capture_output=True, text=True, env=env, cwd=WORKSPACE)
            pid_file = os.path.join(WORKSPACE, 'recomp', 'run', 'ctl-' + name, 'pid')
            try:
                pid = int(open(pid_file).read())
            except (OSError, ValueError):
                pid = None
            if pid:
                self.watch = Watch(pid, self.args.max_secs, self.args.max_rss_mb)
                self.watch.start()
            try:
                p = subprocess.run([sys.executable, routes, name, what], capture_output=True, text=True, env=env,
                                   cwd=WORKSPACE, timeout=self.args.max_secs)
                result = 'route done: ' + (p.stdout.strip().split('\n')[-1] if p.stdout.strip() else '')
                open(os.path.join(work, 'route.out'), 'w').write(p.stdout + p.stderr)
            except subprocess.TimeoutExpired:
                result = 'stopped at the time limit (%d s)' % self.args.max_secs
            finally:
                if pid:
                    try:
                        os.kill(pid, signal.SIGTERM)
                        for _ in range(20):
                            time.sleep(0.25)
                            os.kill(pid, 0)
                        os.kill(pid, signal.SIGKILL)
                    except OSError:
                        pass
            logfile = os.path.join(WORKSPACE, 'recomp', 'logs', name + '.log')
            try:
                shutil.copy(logfile, os.path.join(work, name + '.log'))
            except OSError:
                pass
        took = time.time() - t0
        watch = getattr(self, 'watch', None)
        if watch:
            watch.done.set()
            if watch.why:
                result += '; stopped at %s' % watch.why
        try:
            text = open(logfile, errors='replace').read()
        except OSError:
            text = ''
        missing = re.findall(r'\[fncheck\] (\S+): this build has no traced original', text)
        if missing:
            result += '; NOT RECORDED (no traced original: re-run cmake on the build after adding registrations): ' + \
                ', '.join(missing[:8])
        # The runner's registry differs from the --list the run was planned
        # from (the build was rebuilt from other sources while it ran).
        unknown = sorted(set(re.findall(r'\[fncheck\] ZX_FNREC: not a registered function: (\S+)', text)))
        if unknown:
            result += '; NOT RECORDED (the runner does not register them: rebuilt from other sources?): ' + \
                ', '.join(unknown[:8]) + (' and %d more' % (len(unknown) - 8) if len(unknown) > 8 else '')
        vsync = None
        try:
            tail = open(logfile, 'rb').read()[-262144:].decode('latin-1')
            ticks = re.findall(r'\[(?:state|frame:upload)\][^\n]*?\btick=(\d+)', tail)
            vsync = int(ticks[-1]) if ticks else None
        except OSError:
            pass
        peak = watch.peak if watch else None
        self.log('%s %s: %d functions, %.0f s, last vsync %s, peak %s MB; %s' % (
            self.route, self.pass_name, len(self.addresses), took, vsync, peak, result))
        return {'route': self.route, 'pass': self.pass_name, 'seconds': round(took), 'functions': len(self.addresses),
                'vsync': vsync, 'peak_rss_mb': peak, 'result': result, 'when': time.strftime('%Y-%m-%d %H:%M')}


def cmd_record(args, version_root):
    functions = registered(args.build)
    routes = args.routes or list(ROUTES)
    for r in routes:
        if r not in ROUTES:
            sys.exit('readable_corpus.py: no route %s (routes: %s)' % (r, ', '.join(ROUTES)))
    if args.only:
        # A top-up: only these functions (new Planned or Draft ones), into the
        # same route folders; the other functions' files stay as they are.
        want = set()
        by_key = {}
        for a, n, s in functions:
            for key in (n, '0x%x' % a, '0x%08x' % a):
                by_key.setdefault(key, []).append(a)
        items = args.only
        if items.startswith('@'):
            items = ','.join(l.split('#')[0].strip() for l in open(items[1:]) if l.split('#')[0].strip())
        for item in items.split(','):
            item = item.strip()
            hit = by_key.get(item) or by_key.get(item.lower())
            if not hit:
                sys.exit('readable_corpus.py: %s is not registered in %s' % (item, args.build))
            want.update(hit)
        functions = [f for f in functions if f[0] in want]
    light, heavy = split_heavy(functions, args.store)
    # A top-up of functions under check (--only): their rare paths matter
    # (gamehand4: railFilter's double-rail pair test, past the heavy cap of
    # 50 calls a folder): the light caps, unless --caps names theirs.
    if args.only and not args.heavy_only:
        light, heavy = light + heavy, []
    args.caps_of = parse_caps(args.caps)
    deep = deep_functions(args.store)
    lock = threading.Lock()
    runs_file = os.path.join(version_root, 'runs.json')
    os.makedirs(version_root, exist_ok=True)

    def log(msg):
        with lock:
            line = '%s %s' % (time.strftime('%H:%M:%S'), msg)
            print(line, flush=True)
            open(os.path.join(version_root, 'record.log'), 'a').write(line + '\n')
    log('recording %d routes, %d light and %d heavy functions, %d at a time; %d recorded on %s only (%s)' % (
        len(routes), len(light), len(heavy), args.jobs, len(deep), ', '.join(DEEP_ROUTES), ', '.join('0x%x' % a for a in sorted(deep))))
    passes = args.passes.split(',')
    todo = []
    for r in routes:
        for p, adds in (('all', light + heavy), ('light', light), ('heavy', heavy)):
            if p not in passes:
                continue
            adds = [a for a in adds if r in DEEP_ROUTES or a not in deep]
            if adds:
                todo.append((r, p, adds))
    results = []

    def worker():
        while True:
            with lock:
                if not todo:
                    return
                route, pass_name, adds = todo.pop(0)
            folder = os.path.join(version_root, route)
            res = Run(args, route, pass_name, adds, folder, log, heavy).go()
            with lock:
                results.append(res)
                old = json.load(open(runs_file)) if os.path.exists(runs_file) else []
                old.append(res)
                json.dump(old, open(runs_file, 'w'), indent=1)
    threads = [threading.Thread(target=worker) for _ in range(max(1, args.jobs))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    cmd_index(args, version_root)


def cmd_index(args, version_root, functions=None):
    if functions is None:
        functions = registered(args.build)
    names = {a: (n, s) for a, n, s in functions}
    index = {'version': int(os.path.basename(version_root).lstrip('v')), 'routes': {}, 'when': time.strftime('%Y-%m-%d %H:%M')}
    covered = {}
    total_bytes = 0
    for route in sorted(os.listdir(version_root)):
        folder = os.path.join(version_root, route)
        if not os.path.isdir(folder) or route == 'work':
            continue
        rows = fnrec_index.read_folder(folder)
        if not rows:
            continue
        info = {'functions': {}, 'calls': 0, 'bytes': 0, 'description': ROUTES.get(route, ('', '', 0, {}, ''))[4]}
        for r in rows:
            a = r.get('address')
            if a is None or not r['calls']:
                continue
            info['functions']['%08x' % a] = {'name': names.get(a, ('?', '?'))[0], 'calls': r['calls'],
                                             'callee_calls': r['callee_calls'], 'mb': round(r['bytes'] / 1e6, 2),
                                             'vsyncs': [r['vsync_first'], r['vsync_last']], 'cut_short': r['damaged']}
            info['calls'] += r['calls']
            covered[a] = covered.get(a, 0) + r['calls']
        info['bytes'] = sum(os.path.getsize(os.path.join(folder, f)) for f in os.listdir(folder))
        total_bytes += info['bytes']
        index['routes'][route] = info
    index['bytes'] = total_bytes
    index['registered'] = len(functions)
    index['covered'] = len([a for a, _, _ in functions if a in covered])
    index['uncovered'] = [{'address': '%08x' % a, 'name': n, 'status': s} for a, n, s in functions if a not in covered]
    json.dump(index, open(os.path.join(version_root, 'index.json'), 'w'), indent=1)
    lines = ['Recording corpus v%d (%s): %d of %d registered functions recorded, %.2f GB' % (
        index['version'], index['when'], index['covered'], index['registered'], total_bytes / 1e9)]
    for route, info in index['routes'].items():
        lines.append('  %-12s %4d functions %8d calls %7.2f GB  %s' % (route, len(info['functions']), info['calls'],
                                                                    info['bytes'] / 1e9, info['description']))
    by_status = {}
    for u in index['uncovered']:
        by_status.setdefault(u['status'], []).append(u)
    lines.append('  uncovered: %d (%s)' % (len(index['uncovered']), ', '.join('%s %d' % (k, len(v)) for k, v in sorted(by_status.items()))))
    for u in index['uncovered']:
        lines.append('    0x%s %-9s %s' % (u['address'], u['status'], u['name']))
    open(os.path.join(version_root, 'index.txt'), 'w').write('\n'.join(lines) + '\n')
    print('\n'.join(lines[:len(index['routes']) + 2]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['record', 'index', 'status', 'folders', 'routes', 'situations'])
    ap.add_argument('routes', nargs='*')
    ap.add_argument('--store', default=os.path.join(SCRATCH_ROOT, 'reccorpus'))
    # (no default: a worktree's own build; never the main checkout's, which the
    # merges rebuild: the coordinator, drafter25)
    ap.add_argument('--build')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--max-secs', type=int, default=600)
    ap.add_argument('--max-rss-mb', type=int, default=8000, help='stop a runner above this resident memory')
    ap.add_argument('--passes', default='all', help='which passes to record: all (one pass, caps per function; '
                    'the default), or light and heavy (two runs, the old way)')
    ap.add_argument('--routes-py', default=os.environ.get('READABLE_ROUTES_PY') or os.path.join(WORKSPACE, 'tools', 'routes.py'),
                    help='the tools/routes.py to drive the route runs with (default the workspace\'s)')
    ap.add_argument('--new-blocks', metavar='SEED', help='keep only calls that run a block of the original not in SEED '
                    '(fncheck_corpus.py coverage --covered-out) nor in a kept call (ZX_FNREC_NEW_BLOCKS)')
    ap.add_argument('--nb-per-vsync', type=int, default=0, metavar='N', help='--new-blocks: calls recorded a vsync per '
                    'function (default %d, %d for heavy ones)' % (NB_PER_VSYNC, NB_PER_VSYNC_HEAVY))
    ap.add_argument('--every-call', action='store_true', help='--new-blocks: try every call (ZX_FNREC_NEW_BLOCKS=2: '
                    'no rest, no per-vsync cap or pick; for a few hot functions with --only)')
    ap.add_argument('--mash-coverage', metavar='FILE', help='mash weighted by the uncovered blocks in FILE '
                    '(fncheck_corpus.py coverage --json; routes.py MASH_COVERAGE)')
    ap.add_argument('--only', help='record only these functions (names or addresses, comma separated, or @file): a top-up '
                    '(with the light caps, CAPS_LIGHT, also for heavy functions; --heavy-only keeps theirs)')
    ap.add_argument('--heavy-only', action='store_true', help='--only: heavy functions keep the heavy caps')
    ap.add_argument('--caps', help='calls a folder and a vsync per function, over the defaults (the one-pass mode): '
                    'ADDR:MAX[:PER_VSYNC],... or @file (one a line)')
    args = ap.parse_args()
    args.store = os.path.abspath(args.store)
    if not args.build:
        sys.exit('readable_corpus.py: --build DIR (your worktree\'s build with the runner and zx-fncheck; '
                 'not the main checkout\'s)')
    args.build = os.path.abspath(args.build)
    version_root = os.path.join(args.store, 'v%d' % recorder_version())
    if args.command == 'routes':
        for name, (kind, what, stop, extra, desc) in ROUTES.items():
            print('%-12s %s\n%14s%s: %s%s' % (name, desc, '', kind, what, (' (to vsync %d)' % stop) if stop else ''))
        return
    if args.command == 'situations':
        by = {}
        for name in ROUTES:
            by.setdefault(situation(name), []).append(name)
        for sit, names in by.items():
            played = sorted(set(c for n in names for c in SITUATIONS.get(n, (0, 0, []))[2]))
            print('%s\n    %s%s' % (sit, ', '.join(names), ('\n    challenges: ' + '; '.join(played)) if played else ''))
        return
    if args.command == 'record':
        cmd_record(args, version_root)
    elif args.command == 'index':
        cmd_index(args, version_root)
    elif args.command == 'status':
        path = os.path.join(version_root, 'index.txt')
        print(open(path).read() if os.path.exists(path) else 'no index at %s' % path)
    elif args.command == 'folders':
        if not args.routes:
            sys.exit('readable_corpus.py folders FUNCTION')
        index = json.load(open(os.path.join(version_root, 'index.json')))
        f = args.routes[0].lower()
        for route, info in index['routes'].items():
            for a, fn in info['functions'].items():
                if f in (a, '0x' + a, '0x' + a.lstrip('0'), fn['name'].lower()):
                    print('%s  %d calls' % (os.path.join(version_root, route), fn['calls']))


if __name__ == '__main__':
    main()
