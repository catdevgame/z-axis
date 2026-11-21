#!/usr/bin/env python3
"""The career A/B: careercheck's input in two runs, compared field by field and word by word.

  career_ab.py run NAME --dir DIR [--spec SPEC] [--to TICKS] [--runner R] [--secs S] [--every N]
                        [--input career|logic|frontend|messages|split|editor|crane|pisser|vacuum|FILE]
                        [--packet-dump FROM:TO] [--clock K]
  career_ab.py compare A B [--out FILE] [--list N] [--allow FILE] [--dumps-only|--state-only]
  career_ab.py gate --dir DIR [--runner R] [--clock K] [--inputs a,b,...] [--jobs N] [--allow FILE] [--keep] [--voices] [--game dm2|bmx] [--spec SPEC] [--every N]
  career_ab.py stale --dir DIR [--runner R] [--clock K] [--inputs a,b,...] [--jobs N] [--list N] [--game dm2|bmx]
  career_ab.py script TRACE OUT [KEY=VAL ...]
  career_ab.py packets A B TICK STREAM

`run` runs the input unpaced with the null GS into DIR/NAME (ZX_READABLE=SPEC when given, the
defaults otherwise): a state log in DIR/NAME/state.log, a RAM dump every N ticks (default 3,000), the
scratchpad's hash at every vsync, and per vsync a hash of what rendering is handed
(ZX_PACKET_LOG, runtime/ps2_state_probe.h: the VIF1, GIF and VIF0 streams, the GS register writes)
and of VU0's registers. A dump is 32 MB: delete a run's dumps when done.

The inputs: `career` and `logic` are careercheck's and logiccheck's. The others are pad scripts in
ab_inputs/ next to this file (or any FILE), made by `script` from a closed-loop routes.py run with
ZX_PAD_TRACE=<file> (the buttons and sticks the game read, by tick): tools/routes.py waits on the
game through a client on the host, so its timing is the host's; the script it leaves replays the
same presses at the same ticks. Such a file is `#env KEY=VAL` lines and the script. A route that
pokes the game (a teleport to a giver): ZX_POKE_TRACE=<file> too, and `script TRACE OUT --pokes
FILE`: the pokes go in as ZX_POKE_SCRIPT, the same writes at the same vsyncs. `#env
ZX_ROUTES_CARD=DIR` starts from a copy of that card (under recomp/run), as routes.py start does.

`compare` takes two such run directories (a NAME.log with [state] lines alone also works; its
mangled lines are then counted and listed as not compared) and reports:

  - the state log: every field at every tick from the first line to the last tick both reached.
    The probe writes a line only when a field changed, so a run's value at a tick is its last line
    at or before that tick: comparing only the ticks both logged (the old statecmp.py) misses a field
    that changes in one run and not in the other.
  - the scratchpad: its hash at every vsync both runs have.
  - RAM: every word of every dump the two runs share, by region; with --out every differing word.
  - rendering: the hashes of what it is handed, at every vsync both runs have. `packets` finds the
    first differing word of a tick's stream in two runs made with --packet-dump.
  - what it did not compare: ticks past the shorter run, ticks a mangled line hides, dumps only one
    run has, and the state no dump or log holds (printed each time).

Nothing is left out by default. `--allow FILE[,FILE...]` names words expected to differ, each under its
reason: counted apart, never dropped. Exit 0 only if nothing differs outside the allowed list and
nothing that should have been compared was skipped. A line (see load_allow) is `ADDR[+BYTES] [OPTION
...] reason`; the options make it as narrow as its proof:

    within=N     the two values are at most N apart (a clock word: a difference of the time read)
    every=S at=OFF[,OFF]   only those offsets of records S bytes apart
    bits=MASK    only those bits of the word differ (a field whose upper bits nothing reads)
    stack        words of a stack no thread's sp is in at that dump (its thread has ended; interrupts' stack)

A word that is a field of a record has no address of its own: heaps move with the input. Such a line has no ADDR:

    owner=NAME [bits=MASK] reason

A leftover vector lane (a stale w, a spilled upper half) proven by wlane.py (`wlane.py poke`: the lane forced to two
values on one input moves these RAM words and nothing observable, and every load of them is followed to a discard):

    lanes=FILE input=NAME reason

FILE (relative to the allow file) is the poke's `.lanes.txt`: `tTICK 0xADDR ...` lines; a word passes at the dump
with that label alone, and only when `compare` is looking at runs of that input (run.txt).

A setting that hands the GS other work for the same frames (render_path = records: ab_allow_records.txt) names the
streams it changes, whose differing vsyncs are then counted apart under the reason (the RAM behind them is not):

    stream=NAME[,NAME] reason

Its words come from walking the game's own structures in the dump (OWNERS), on each side at each dump: a word
passes only if the walk finds it on both sides, and only in `bits`. A word the walk does not find fails, so a heap
word that holds something else at another dump is compared. The frames of a stack are the same: `frame` lines
(load_allow) are a table of functions, owner=frames walks each thread's chain of saved return addresses through that
table in the `off` dump, and only a chain that reaches its stack's top counts (owner rules: OWNERS, some for a record left in
dead memory, which no list reaches: those test the record itself, and the same word holding anything else is compared). A frame's `saves=` are the slots its
prologue writes and only its epilogue reads; a readable function has no epilogue, so they hold what they held.

    frame NAME START+BYTES stack=STACK size=S ra=OFFSET saves=OFF,OFF [last] [unused=OFF,OFF] [string=OFF+BYTES] reason

`stack`: the stack it is proven on. `last`: the thread's first function. `unused=`: words nothing touches (proof in the reason). `string=`: a buffer read
as a C string: a word past its NUL on both sides.

Dead stack is not a line: a word of a guest thread's stack below that thread's sp in both runs at the
dump (the dump's .sp file) is counted apart as dead stack: nothing can read it before writing it. A
thread whose record does not say where its stack is (main: SetupThread's values, sp under them) gets a
`stack-of=ID` line with the range.

`--clock K` runs with ZX_EE_SCALE=K: the EE K times faster than the clock it is charged against,
idle jumps whole, so no frame overruns its vsync and two sides that charge different cycles for the
same work stay tick for tick together. What differs then is logic, not time. Two runs compare only
at the same clock (`compare` says so and fails otherwise); a run's clock is in its run.txt.

`stale` is the stale-stack check. A value a function leaves on the stack and a later function reads has no gate of
its own: the replay answers calls, and the A/B sees two runs whose stacks hold whatever came before, which can match by
luck. So each input runs the A/B again with the stack filled with 0x5A at the entry of every function of
readable_frame_stores.tsv (each function's frame size in the original and the slots it stores there and, on no path,
reads before storing), on BOTH sides,
readable or recompiled original alike (ee/abi.cpp installStackFill): `off` against the defaults, compared as `gate`
does (ab_allow.txt), and the arguments of every voice call on top. Two fills (--fills):

  - stores (ZX_STACK_POISON_STORES, DIR/NAME-dp-off, -dp-on): only the slots the function stores in the original. A
    store the readable keeps writes over the fill; one it drops leaves 0x5A where `off` has the original's value, and
    a later reader (hand-assets' pointVelocity: a transpose's w lanes through VU0 to the camera; hand-level2's
    pairDoubleRailEdge: an address bonePoint reads as a row's w at the museum; drawWorldPrimitives' place, which
    drawSparks measures) takes it. The table leaves out every slot a function reads before storing on some path (its
    control flow, readable_frame_stores.py): such a reader's own entry fill would write over the value left for it on
    both sides and hide the dropped store, as it did for those three before the table followed paths. The whole
    frame's fill cannot see this at all: the reader's own entry fills the slot either way.
  - frames (ZX_STACK_POISON_FRAMES, DIR/NAME-pz-off, -pz-on): the whole frame. A read of a slot the function never
    writes (stale1's soundEvent 30, drawSparks).

What the original itself reads stale (a slot stored on another path: ramDiskSeek's report when there is no callback,
which the camera's w lanes take; the byte soundEvent 30 takes from playerUpdate's frame) reads the same fill on both
sides, so it is no difference: an earlier `stale` filled the defaults alone and failed on those by construction. Not
seen: a store a callee makes through a pointer into the frame (the table has no extent for it), and a function whose
readable has no frame layout called as plain C++ from another readable one (enterFrame cannot find its row).
Report: DIR/NAME.dropped.txt (stores), DIR/NAME.stale.txt (frames); a pass deletes the dumps (--keep keeps them).

`gate` is the merge's A/B: every input (GATE_INPUTS) with `off` on one side and the defaults on the
other, same runner, both at --clock (default 4096), at most --jobs runs at a time (default 2,
waiting for 30% free memory), then `compare` with ab_allow.txt. One line per input; a pass deletes
its dumps (--keep keeps them). Exit 0 only if every input passes.

DM2's and BMX XXX's `menus` input (PAGE_INPUTS) is the app's Settings page in the game's own menus, from Options
and from the pause menu (ZX_GAME_SETTINGS_MENUS=1 on both sides, the route's), every section, Right then Left on
every row. With the route's ZX_TRACE_SETTINGS=1 its log has the page's keys (zge_settings.cpp: each key, the row,
its value before and as the page then shows it): `compare` reads them back on each side (a Left finds the value its
Right showed and, if the Right changed it, shows the one before again; page_readback) and wants the two sides' keys
the same; a run that never reaches the page fails.
"""
import argparse
import array
import collections
import fcntl
import glob
import json
import os
import re
import struct
import shutil
import subprocess
import sys
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

# Mode, panel, level frame, position, level clock, the camera's eye and look-at (all four lanes),
# the music's track, the movers' list.
DEFAULT_FIELDS = '0x276FE4,0x240A20,0x231CAC,0x2A76F0:3,0x276FF8,0x24C9E0:8,0x279668,0x2774C4'
RAM_REGIONS = [  # the ELF's one segment and what the game keeps above it (a report's rough map)
    (0x00000000, 0x00100000, 'below the ELF'),
    (0x00100000, 0x00279158, 'ELF text and data'),
    (0x00279158, 0x002B3B70, 'bss'),
    (0x002B3B70, 0x01EF0000, 'heap'),
    (0x01EF0000, 0x01F00000, 'main stack'),
    (0x01F00000, 0x02000000, 'above the main stack'),
]
NOT_HELD = ('GPRs and FPU registers, IOP and SPU2 state, DMA and timers; VU memory, the GS registers and '
            'VRAM only through the streams that make them (a VU1 microprogram does not run: ZX_VU1_SKIP)')
PACKET_STREAMS = ('vif1', 'gif', 'vif0', 'gs')
# The merge gate's inputs and how far each goes (where its last route ends).
GATE_INPUTS = (('career', 20000), ('logic', 36000), ('frontend', 31000), ('messages', 23500), ('split', 20000),
               ('editor', 43000), ('crane', 11700), ('pisser', 12200), ('vacuum', 12700))
GATE_INPUTS = GATE_INPUTS + (('play-civic', 21030), ('play-industrial', 21220), ('play-boardwalk', 20367), ('play-cannery', 20839), ('play-airfield', 21449), ('play-museum', 20295), ('play-tutorial', 14695), ('play-movielot', 33163))  # played sessions (tools/playbot/ai.py)
# Industrial's pause menu Save with the app's settings pages on: the save screen in the zgui heap the level shares with its
# ParkEditor.zsc and the pages (the hang of 2026-11-15: a run that stops short of its end fails, below)
GATE_INPUTS = GATE_INPUTS + (('save-industrial', 4000),)
GATE_CLOCK = 4096
# Another game's A/B (--game dm2, bmx): its checks are AI's over its own run script, inputs (routes of
# tools/dm2routes.py, bmxroutes.py in DM2's repository), fields, RAM map and allow list. AI's are the constants above.
GAMES = {'dm2': {
    'fields': '0x259770,0x2566C8,0x25BCC4,0x25A1A8,0x2F4EA0',  # mode, session, level frame, level cursor, the rider table
    'regions': [(0x00000000, 0x00100000, 'below the ELF'), (0x00100000, 0x0025B780, 'ELF text and data'),
                (0x0025B780, 0x003040B0, 'bss'), (0x003040B0, 0x01EF0000, 'heap'), (0x01EF0000, 0x01F00000, 'main stack'),
                (0x01F00000, 0x02000000, 'above the main stack')],
    'gate': (('fr-woodward', 12000), ('fr-trainyards', 12000), ('pq-woodward', 14000), ('se-woodward', 15000), ('frontend', 6000),
             ('editor-0', 20000), ('play-p1-se0', 15292), ('play-p3-fr0', 16782),  # those two played (ab_inputs/dm2-play-*)
             ('menus', 15400)),  # the app's Settings page from Options and the pause menu (ZX_GAME_SETTINGS_MENUS=1, both sides)
    # (with render_path = records, the defaults' native drawing since nativedef1: the records' own list too)
    'allow': 'ab_allow_dm2.txt,ab_allow_records_dm2.txt',
    'cycles': '1e79c8',  # any traced function will do: __divdi3
    'routes': 'dm2routes.py',
}, 'bmx': {
    'fields': '0x29D040,0x2A2DE0,0x2F3A30',  # mode, level frame, the rider table
    'regions': [(0x00000000, 0x00100000, 'below the ELF'), (0x00100000, 0x002A2680, 'ELF text and data'),
                (0x002A2680, 0x00314B30, 'bss'), (0x00314B30, 0x01EF0000, 'heap'), (0x01EF0000, 0x01F00000, 'main stack'),
                (0x01F00000, 0x02000000, 'above the main stack')],
    'gate': (('frontend', 9000), ('s-dam', 10202), ('s-rmpg', 11061), ('s-root', 11459), ('s-muly2', 7562), ('s-dam2-end', 10922),
             ('menus', 16300),  # (as DM2's)
             # the park editor's SESSION with a saved park (custom0's six-zone park, a native save: ab_inputs/bmx-pe-session),
             # played by bikepark.py past the session's two-minute clock and PLAY AGAIN: the park list through
             # saveSystemTask's file menu (BMX XXX's kind 16), the park built, ridden, the game over screen (bmxbot1: no
             # other input loads a saved park, and the readable saveSystemTask's wrong kinds reached the main build)
             ('pe-session', 11600)),
    # (bmxbot1) pe-session's RAM words are leftover w lanes (GeoObject bounds, cameras, ray hits: stack a whole
    # register's spill leaves, which the readable frames do not hold) whose readers are not proven yet: reported, not
    # counted, until the w-lane dataflow proof lands. Its fields, packets and scratchpad are counted, and the park must
    # load on both sides.
    'ram_information': ('pe-session',),
    'log_lines': {'pe-session': (('[saves] park load: read ', True), ('missing or damaged', False))},
    'allow': 'ab_allow_bmx.txt,ab_allow_records_bmx.txt',  # (as DM2's)
    'cycles': '23dc00',  # __divdi3
    'routes': 'bmxroutes.py',
    # The voice calls at BMX's addresses (`stale`): the high pairs of AI's (workdb card), the same arguments. stopVoice,
    # sourceHeard and soundEvent have no sure pair, so they are not logged.
    'voices': {0x1118F0: ('allocateVoice', (0, 1)), 0x111DC8: ('releaseVoice', (0,)), 0x111DF0: ('playSample', (0, 1, 2, 3)),
               0x111ED0: ('startVoice', (0,)), 0x1120B8: ('setVoiceVolume', (0, 1)), 0x112128: ('setVoicePan', (0, 1)),
               0x1121B8: ('setVoicePitch', (0, 1))},
}}

# The voice calls, with the arguments the callee takes (`stale`).
VOICE_CALLS = {0x112480: ('allocateVoice', (0, 1)), 0x112948: ('releaseVoice', (0,)), 0x112970: ('playSample', (0, 1, 2, 3)),
               0x112A50: ('startVoice', (0,)), 0x112AD0: ('stopVoice', (0,)), 0x112C50: ('setVoiceVolume', (0, 1)),
               0x112CC0: ('setVoicePan', (0, 1)), 0x112D50: ('setVoicePitch', (0, 1)), 0x16FAE0: ('sourceHeard', (0,)),
               0x1C2558: ('soundEvent', (0,))}
# `stale`'s fills: the run folders' tag, `run`'s flag, the report's suffix.
STALE_FILLS = {'stores': ('dp', '--poison-stores', '.dropped.txt'), 'frames': ('pz', '--poison-frames', '.stale.txt')}
def voice_table(game):
    """The voice calls `stale` logs in a game: AI's VOICE_CALLS, another game's own table (none if it has none)."""
    return VOICE_CALLS if game == 'ai' else GAMES[game].get('voices', {})


PAD_BUTTONS = {0x0001: 'SELECT', 0x0002: 'L3', 0x0004: 'R3', 0x0008: 'START', 0x0010: 'UP', 0x0020: 'RIGHT',
               0x0040: 'DOWN', 0x0080: 'LEFT', 0x0100: 'L2', 0x0200: 'R2', 0x0400: 'L1', 0x0800: 'R1',
               0x1000: 'TRIANGLE', 0x2000: 'CIRCLE', 0x4000: 'CROSS', 0x8000: 'SQUARE'}


def main_repo():
    return os.environ.get('AI_MAIN') or zaxis_paths.get('AI_WORKSPACE')


def dm2_repo():
    return os.environ.get('AI_DM2') or zaxis_paths.get('DM2_WORKSPACE')


def hold(path, what):
    """An flock on PATH for as long as this process lives, or exit 75 when another process holds it (tools30: b126's
    two merge-checks ran one gate's runs and compares over the other's, which deleted the dumps a compare was reading;
    with this a second gate or run of the same folder stops at once, naming the first)."""
    f = open(path, 'a+')
    try:
        fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        f.seek(0)
        print('career_ab: %s is in use by another career_ab (%s): exit 75' % (what, f.read().strip() or path), flush=True)
        sys.exit(75)
    f.seek(0)
    f.truncate()
    f.write('pid %d: %s\n' % (os.getpid(), ' '.join(sys.argv[1:])[:200]))
    f.flush()
    return f


def run(args):
    main = main_repo()
    root = os.path.abspath(args.dir)
    os.makedirs(root, exist_ok=True)
    held = hold(os.path.join(root, '.%s.lock' % args.name), 'run %s in %s' % (args.name, root))  # noqa: F841 (held to exit)
    other = GAMES.get(args.game)
    fields = args.fields or (other['fields'] if other else DEFAULT_FIELDS)
    out = os.path.join(root, args.name)
    subprocess.run(['rm', '-rf', out], check=True)
    os.makedirs(out)
    env = dict(os.environ)
    cards = None
    if other:
        env['GAME'] = args.game
        env.update({'ZX_DEV_UNLOCK_ALL': '1', 'ZX_MC_FD_BASE0': '1', 'ZX_MC_FIXED_TIME': '1'})  # (the card as dm2corpus.py records it)
        if args.game == 'dm2':
            env['ZX_PACKET_DEAD_W'] = '1'  # (proven for DM2's shadow volumes' point blocks: tools34, tested2)
        if PASSTHROUGH_PROGRAMS.get(args.game):  # (the GS's unread PACKED bits of what a passthrough program sends)
            env['ZX_PACKET_PASSTHROUGH'] = ','.join('0x%X' % p for p in PASSTHROUGH_PROGRAMS[args.game])
        played = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ab_inputs', args.game + '-' + args.input + '.txt')
        if os.path.exists(played):  # a played route kept with the gate (ab_inputs): its script and environment
            with open(played) as f:
                lines = f.read().splitlines()
            script = ''.join(line for line in lines if not line.startswith('#'))
            pairs = [line[5:] for line in lines if line.startswith('#env ')]
        else:
            routes = os.path.join(dm2_repo(), 'tools', other['routes'])
            script = subprocess.check_output([sys.executable, routes, 'script', args.input], text=True)
            pairs = subprocess.check_output([sys.executable, routes, 'env', args.input], text=True).splitlines()
        own = {}
        for line in pairs:
            key, _, value = line.partition('=')  # the route's own environment, but for the clock (--clock's)
            if key == 'ZX_SAVES_DIR' and value.startswith('@'):
                # a saves folder kept with the input (ab_inputs/<name>: BMX XXX's park), copied into the run's folder
                # so the run writes only its own copy
                saves = os.path.join(out, 'saves')
                shutil.copytree(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ab_inputs', value[1:]), saves)
                value = saves
            if key != 'ZX_EE_SCALE':
                env[key] = value
                own[key] = value
        if len(script) > 100000:  # a played route (tools/playbot): longer than an environment string can be
            path = os.path.join(out, 'script.txt')
            with open(path, 'w') as f:
                f.write(script)
            script = '@' + path
    elif args.input == 'career':
        script = subprocess.check_output([sys.executable, os.path.join(main, 'tools', 'careercheck.py'), 'script'], cwd=main, text=True)
    elif args.input == 'logic':  # logiccheck's input (the Tutorial's moves, other paths than the career's)
        script = subprocess.check_output([sys.executable, '-c', 'import logiccheck; print(logiccheck.native_script())'],
                                         cwd=os.path.join(main, 'tools'), text=True)
    else:
        path = args.input if os.path.exists(args.input) else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'ab_inputs', args.input + '.txt')
        script = ''
        with open(path) as f:
            for line in f:
                if line.startswith('#env '):
                    key, _, value = line[5:].strip().partition('=')
                    if key == 'ZX_ROUTES_CARD':
                        cards = os.path.join(main, 'recomp', 'run', value)
                    else:
                        env[key] = value
                elif not line.startswith('#'):
                    script += line.strip()
    ticks = range(args.every, args.to + 1, args.every) if args.every else []
    env.update({
        'RUN_DIR': root, 'RUNNER': args.runner, 'ZX_PAD_SCRIPT': script.rstrip('\n'), 'ZX_PAD_SCRIPT_CLOCK': 'vsync',
        'ZX_HOST_INPUT': '0', 'ZX_GS_NULL': '1', 'ZX_VU1_SKIP': '1', 'ZX_SAVES': 'card', 'STOP_TICK': str(args.to),
        'ZX_STATE_LOG': fields, 'ZX_STATE_LOG_FILE': os.path.join(out, 'state.log'),
        'ZX_DUMP_PREFIX': os.path.join(out, 'dump'),
        'ZX_PACKET_LOG': os.path.join(out, 'packets.txt'),
        # The scratchpad's hash at every vsync (the cycle tracer's watch; it needs one traced function).
        'ZX_FNCYCLES': (other['cycles'] if other else '18fc70') + ':' + os.path.join(out, 'fncycles.txt'),
        'ZX_FNCYCLES_WATCH': '70000000+16384:' + os.path.join(out, 'scratchpad.txt'),
    })
    if other and own.get('ZX_SAVES'):
        env['ZX_SAVES'] = own['ZX_SAVES']  # an input of the native saves (pe-session: a park saved as a file)
    if args.packet_dump:
        env['ZX_PACKET_DUMP'] = args.packet_dump
    if args.voices:  # (the call log's own: vsync numbers and scratchpad as without it)
        env['ZX_FNCYCLES'] = ','.join([other['cycles'] if other else '18fc70'] + ['%x' % a for a in voice_table(args.game)]) + ':' + os.path.join(out, 'fncycles.txt')
        env['ZX_FNCYCLES_ARGS'] = os.path.join(out, 'voice.txt')
    if args.poison:
        env['ZX_STACK_POISON'] = '1'
    table = args.stores or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'readable_frame_stores.tsv')
    if args.poison_stores:
        env['ZX_STACK_POISON_STORES'] = table
    if args.poison_frames:
        env['ZX_STACK_POISON_FRAMES'] = table
    env.pop('ZX_EE_SCALE', None)
    if args.clock != 1:
        env['ZX_EE_SCALE'] = str(args.clock)
    if ticks:
        env['ZX_DUMP_WHEN'] = ','.join('t%d:tick==%d' % (t, t) for t in ticks)
    if args.spec is not None:
        env['ZX_READABLE'] = args.spec
    with open(os.path.join(out, 'run.txt'), 'w') as f:
        f.write('runner %s\nspec %s\nto %d\ninput %s\nfields %s\nclock %d\n' % (args.runner, args.spec, args.to, args.input, fields, args.clock))
        if other:
            f.write('game %s\n' % args.game)
    if cards:  # (run-game.sh keeps a card that is there)
        shutil.copytree(cards, os.path.join(out, 'mc0'))
    run_game = os.path.join(dm2_repo(), 'tools', 'run-game-dm2.sh') if other else os.path.join(main, 'tools', 'run-game.sh')
    r = subprocess.run([run_game, args.name, str(args.secs), args.runner], env=env)
    subprocess.run(['rm', '-rf', os.path.join(out, 'home'), os.path.join(out, 'app')])
    return r.returncode


class StateLog:
    """A run's [state] lines. `lines`: (tick, {field: value}) in file order. A mangled line (another
    thread's text spliced in) keeps the fields before the splice; `mangled` lists its tick and the
    fields it lost, whose values are then unknown until a whole line."""

    def __init__(self, directory):
        self.path = os.path.join(directory, 'state.log')
        if not os.path.exists(self.path):
            logs = [p for p in glob.glob(os.path.join(directory, '*.log')) if os.path.basename(p) != 'state.log']
            if not logs:
                raise SystemExit('no state.log or *.log in ' + directory)
            self.path = logs[0]
        self.lines = []
        self.mangled = []
        fields = None
        with open(self.path, errors='replace') as f:
            for text in f:
                at = text.find('[state] tick=')
                if at < 0:
                    continue
                spliced = at > 0
                body = text[at + len('[state] '):].rstrip('\n')
                cut = body.find('[')
                if cut >= 0:
                    body = body[:cut]
                    spliced = True
                parts = body.split()
                row = {}
                for part in parts:
                    m = re.fullmatch(r'([0-9a-z]+)=([0-9a-f]+)', part)
                    if m:
                        row[m.group(1)] = m.group(2)
                    else:
                        spliced = True
                if 'tick' not in row:
                    self.mangled.append((None, []))
                    continue
                tick = int(row.pop('tick'), 10)
                if fields is None and not spliced:
                    fields = list(row)
                if fields is not None and len(row) < len(fields):
                    spliced = True
                    # (A value cut by the splice can look whole: it is not trusted.)
                    if row:
                        row.pop(list(row)[-1])
                if spliced:
                    self.mangled.append((tick, [k for k in (fields or []) if k not in row]))
                self.lines.append((tick, row))
        self.fields = fields or []
        self.last = max((t for t, _ in self.lines), default=0)


def values_at_every_tick(log, field, last_tick):
    """(start, end, value) runs of `field`, carried forward; value None while unknown."""
    runs = []
    value = None
    start = 0
    for tick, row in log.lines:
        if tick > last_tick:
            break
        new = row.get(field)  # missing: lost to a splice, unknown until the next whole line
        if new != value:
            if tick > start:
                runs.append((start, tick, value))
            start = tick
            value = new
    if last_tick + 1 > start:
        runs.append((start, last_tick + 1, value))
    return runs


def compare_field(a, b):
    """Merges two run lists; returns (ticks differing, unknown ticks, first (tick, va, vb), last tick, intervals)."""
    i = j = 0
    differ = unknown = 0
    first = None
    last = None
    intervals = 0
    previous_end = None
    while i < len(a) and j < len(b):
        start = max(a[i][0], b[j][0])
        end = min(a[i][1], b[j][1])
        if end > start:
            va, vb = a[i][2], b[j][2]
            if va is None or vb is None:
                if not (va is None and vb is None and start == 0):
                    unknown += end - start
            elif va != vb:
                differ += end - start
                if first is None:
                    first = (start, va, vb)
                last = end - 1
                if previous_end != start:
                    intervals += 1
                previous_end = end
        if a[i][1] <= end:
            i += 1
        if j < len(b) and b[j][1] <= end:
            j += 1
    return differ, unknown, first, last, intervals


def as_float(text):
    return struct.unpack('<f', struct.pack('<I', int(text, 16)))[0]


def compare_state(a_dir, b_dir, report, problems):
    a, b = StateLog(a_dir), StateLog(b_dir)
    if not a.lines or not b.lines:
        report('state: no lines (%d and %d)' % (len(a.lines), len(b.lines)))
        problems.append('no state lines')
        return
    last = min(a.last, b.last)
    only_a, only_b = set(a.fields) - set(b.fields), set(b.fields) - set(a.fields)
    fields = [f for f in a.fields if f in b.fields]
    report('state: %s lines to tick %d, %s lines to tick %d; %d fields compared at every tick to tick %d' % (
        format(len(a.lines), ','), a.last, format(len(b.lines), ','), b.last, len(fields), last))
    logged = len({t for t, _ in a.lines if t <= last} ^ {t for t, _ in b.lines if t <= last})
    report('  %s ticks have a line in one run only (a change the other run did not have, or a value changed and back)' % format(logged, ','))
    if only_a or only_b:
        report('  NOT COMPARED: fields in one run only: %s' % sorted(only_a | only_b))
        problems.append('fields in one run only')
    if max(a.last, b.last) > last:
        report('  NOT COMPARED: ticks %d to %d (the shorter run stopped)' % (last + 1, max(a.last, b.last)))
    for name, log in (('A', a), ('B', b)):
        if log.mangled:
            lost = sum(1 for _, f in log.mangled if f)
            report('  run %s: %d mangled lines in %s, %d lost fields (unknown until the next line)' % (
                name, len(log.mangled), os.path.basename(log.path), lost))
    same = 0
    total_unknown = 0
    for field in fields:
        d, u, first, end, n = compare_field(values_at_every_tick(a, field, last), values_at_every_tick(b, field, last))
        total_unknown += u
        if d == 0:
            same += 1
            continue
        report('  DIFFERS 0x%s: %s of %s ticks, %d stretches, ticks %d to %d; first A %s (%g) B %s (%g)' % (
            field, format(d, ','), format(last + 1, ','), n, first[0], end, first[1], as_float(first[1]), first[2], as_float(first[2])))
        problems.append('state field 0x%s' % field)
    report('  %d of %d fields equal at every tick' % (same, len(fields)))
    if total_unknown:
        report('  NOT COMPARED: %s field-ticks a mangled line hides' % format(total_unknown, ','))


# The game's structures an owner rule walks (Aggressive Inline's symbols; zgui/library.cpp, level/dynamics.h).
G_UI_LIBRARIES = 0x240CE0     # g_uiLibraries: UiLibrary list through next (+0)
UI_ASSET_LIST, UI_ASSET_KIND, UI_ASSET_TEXTURE_FLAGS = 0xAC, 0xA8, 0xB0
UI_MATERIAL_ASSET, UI_FONT_ASSET = 0, 1
UI_ASSET_NAME_BACK = 0x28     # name, before textureFlags
G_UI_ROOT, NODE_NEXT_SIBLING, NODE_FIRST_CHILD, NODE_SCREEN_POSITION = 0x240A1C, 0x00, 0x04, 0x30  # g_uiRoot, zgui::Node
G_DYNAMICS_OBJECTS, G_DYNAMICS_OBJECT_COUNT = 0x2AAB90, 0x279624  # dynamicsObjects, g_dynamicsObjectCount
RECORD_DYNAMICS, DYNAMICS_BODIES, BODY_NEXT = 0x0C, 0x10, 0x3D8
BODY_WORLD_INERTIA_ROW3, BODY_WORLD_INVERSE_ROW3 = 0xC0, 0x150
BODY_WORLD_INERTIA, BODY_WORLD_INVERSE, BODY_LAST_INVERSE, BODY_PUSHED_W = 0x90, 0x120, 0x220, 0x1BC
G_GEOMETRY_NODE_POOL = 0x279344
POOL_USED, POOL_LINK_BYTES = 0x00, 0x10
NODE_LOCAL, NODE_WORLD = 0x10, 0x50
G_FOLLOW_CAMERAS, FOLLOW_CAMERA_BYTES, FOLLOW_CAMERA_PLAYER = 0x24C9D0, 0x80, 0x04
PLAYER_BOB_CHANNELS, CHANNEL_SCALE_W = (0x908, 0x90C), 0x1C
SPLASH_RING_POINTS = 16
HEAPS, HEAP_COUNT, HEAP_BYTES = 0x2984D0, 128, 0x14  # heaps
HEAP_BLOCK_FREE, HEAP_BLOCK_BYTES, HEAP_BLOCK_HEADER = 0x08, 0x0C, 0x10
LONGEST_WALK = 1 << 16


class Dump:
    """A RAM dump's words, and the records the game's own lists hold in it."""

    def __init__(self, data, threads):
        self.data, self.threads = data, threads
        self.found = {}

    def word(self, address):
        return struct.unpack_from('<I', self.data, address)[0] if 0 <= address <= len(self.data) - 4 else 0

    def chain(self, first, next_offset):
        seen = set()
        while first and first not in seen and len(seen) < LONGEST_WALK:
            seen.add(first)
            yield first
            first = self.word(first + next_offset)

    def owned(self, name):
        if name not in self.found:
            self.found[name] = OWNERS[name](self)
        return self.found[name]


def ui_texture_flags(dump):
    """UiAsset.textureFlags of every material and font asset of every loaded library."""
    words = set()
    for library in dump.chain(dump.word(G_UI_LIBRARIES), 0):
        for asset in dump.chain(dump.word(library + UI_ASSET_LIST), 0):
            if dump.word(asset + UI_ASSET_KIND) in (UI_MATERIAL_ASSET, UI_FONT_ASSET):
                words.add(asset + UI_ASSET_TEXTURE_FLAGS)
    return words


def body_inertia_rows(dump):
    """x, y, z of row 3 of worldInertia and of worldInverseInertia, of every body of every dynamics object."""
    words = set()
    for i in range(dump.word(G_DYNAMICS_OBJECT_COUNT)):
        record = dump.word(G_DYNAMICS_OBJECTS + 4 * i)
        for body in dump.chain(dump.word(dump.word(record + RECORD_DYNAMICS) + DYNAMICS_BODIES), BODY_NEXT):
            for row in (BODY_WORLD_INERTIA_ROW3, BODY_WORLD_INVERSE_ROW3):
                words.update(body + row + lane for lane in (0, 4, 8))
    return words


def body_w_lanes(dump):
    """w of worldInertia's, worldInverseInertia's and lastInverseInertia's rows and of pushedPosition, of every body of
    every dynamics object: what matrixMultiply carries over and a bone's point leaves, see ab_allow.txt."""
    words = set()
    for i in range(dump.word(G_DYNAMICS_OBJECT_COUNT)):
        record = dump.word(G_DYNAMICS_OBJECTS + 4 * i)
        for body in dump.chain(dump.word(dump.word(record + RECORD_DYNAMICS) + DYNAMICS_BODIES), BODY_NEXT):
            for matrix in (BODY_WORLD_INERTIA, BODY_WORLD_INVERSE, BODY_LAST_INVERSE):
                words.update(body + matrix + row * 0x10 + 0xC for row in range(3))
            words.add(body + BODY_PUSHED_W)
    return words


def used_pool_records(dump, pool):
    """The records a pool has handed out."""
    return [link + POOL_LINK_BYTES for link in dump.chain(dump.word(pool + POOL_USED), 0)]


def node_w_lanes(dump):
    """w of rows 0 to 2 of local and world of every node of the geometry node pool."""
    words = set()
    for node in used_pool_records(dump, dump.word(G_GEOMETRY_NODE_POOL)):
        for matrix in (NODE_LOCAL, NODE_WORLD):
            words.update(node + matrix + row * 0x10 + 0xC for row in range(3))
    return words


def splash_ring_w(dump):
    """w of a point of a splash ring, alive or freed. A ring has no list of its own and the memory allocateSized falls
    back on keeps no walkable blocks (a freed ring's head goes to the next record first), so the points say what they
    are: the word is lane w of a run of at least 16 quadwords at one height."""
    def still_a_ring(address):
        if address & 0xF != 0xC:
            return False
        height = lambda quad: dump.word(quad + 4)
        first = last = address & ~0xF
        if not height(first):
            return False
        while height(first - 0x10) == height(first):
            first -= 0x10
        while height(last + 0x10) == height(last):
            last += 0x10
        return (last - first) // 0x10 + 1 >= SPLASH_RING_POINTS

    return Test(still_a_ring)


# The save system's message queues (zge/platform/save_system.h SaveMessageQueue: limit, oldest, then `limit` 16-byte
# SaveMessages: kind, value, listBuffer, choice), kQueueMessages each; a slot of kind 0 is free.
SAVE_QUEUE_MESSAGES, SAVE_MESSAGE_BYTES, SAVE_QUEUE_FIRST = 20, 0x10, 0x08
SAVE_MESSAGE_KINDS = range(0, 17)  # 0 free, 2..6 a job's result, 8..16 the message kinds


def save_queue_free_slots(dump):
    """value, listBuffer and choice of a free slot (kind 0) of a save message queue: the save system's two and those
    stopSaveSystem has freed, which nothing points to. A queue says what it is: limit 20, oldest below it, every slot's
    kind 0, a job's result or a message kind; the slot holding the word is free in it."""
    def free_slot_word(address):
        slot = ((address - SAVE_QUEUE_FIRST) & ~0xF) + SAVE_QUEUE_FIRST
        if address - slot not in (4, 8, 12) or dump.word(slot) != 0:
            return False
        for index in range(SAVE_QUEUE_MESSAGES):
            queue = slot - SAVE_QUEUE_FIRST - index * SAVE_MESSAGE_BYTES
            if dump.word(queue) == SAVE_QUEUE_MESSAGES and dump.word(queue + 4) < SAVE_QUEUE_MESSAGES:
                first = queue + SAVE_QUEUE_FIRST
                return all(dump.word(first + i * SAVE_MESSAGE_BYTES) in SAVE_MESSAGE_KINDS for i in range(SAVE_QUEUE_MESSAGES))
        return False

    return Test(free_slot_word)


def bob_scale_w(dump):
    """w of the scale of the two channels skaterSway bobs, of each camera's player."""
    words = set()
    for i in range(2):
        player = dump.word(G_FOLLOW_CAMERAS + i * FOLLOW_CAMERA_BYTES + FOLLOW_CAMERA_PLAYER)
        if player:
            words.update(dump.word(player + field) + CHANNEL_SCALE_W for field in PLAYER_BOB_CHANNELS)
    return words


def past_allocation(dump):
    """{word: the bits of it past the allocation} of a block of a game heap, free or not (a free block keeps its last
    size): what the block's own bytes do not reach."""
    words = {}
    for i in range(HEAP_COUNT):
        blocks = list(dump.chain(dump.word(HEAPS + i * HEAP_BYTES), 0))
        for block, after in zip(blocks, blocks[1:]):
            end = block + HEAP_BLOCK_HEADER + dump.word(block + HEAP_BLOCK_BYTES)
            if end & 3:
                words[end & ~3] = (0xFFFFFFFF << (8 * (end & 3))) & 0xFFFFFFFF
            words.update((at, 0xFFFFFFFF) for at in range((end + 3) & ~3, after, 4))
    return words


def freed_ui_records(dump):
    """textureFlags of a UiAsset freed with its library: in a free block of a game heap, and still that record."""
    spans = []
    for i in range(HEAP_COUNT):
        blocks = list(dump.chain(dump.word(HEAPS + i * HEAP_BYTES), 0))
        spans += [(block, after) for block, after in zip(blocks, blocks[1:]) if dump.word(block + HEAP_BLOCK_FREE)]

    def still_a_record(address):
        if not any(start <= address < end for start, end in spans):
            return False
        name = dump.data[address - UI_ASSET_NAME_BACK:address - UI_ASSET_NAME_BACK + 0x20].split(b'\0')[0]
        return dump.word(address - 8) in (UI_MATERIAL_ASSET, UI_FONT_ASSET) and name != b'' and name.decode('latin-1').isprintable()

    return Test(still_a_record)


# BMX XXX's Shard (game/bmx/fx/shards.h): the particle's flags at +0 (kParticleMoves, 1), its three corners from +0xB0
# (0x20 bytes each: position, then u and v at +0x10, +0x14), the parameters' flags at +0x110; the health bar's shards'
# flags 5 (game/bmx/overlay/displays.cpp kHealthShardFlags: on screen, a second draw), material 0.
BMX_SHARD_MOVES, BMX_SHARD_CORNERS, BMX_SHARD_CORNER_BYTES, BMX_SHARD_FLAGS, BMX_HEALTH_SHARD = 1, 0xB0, 0x20, 0x110, 5


def bmx_health_shard_texcoords(dump):
    """render_path = records in BMX XXX: a corner's u or v of a health bar shard, alive or freed (no list reaches a freed
    one, and later records may have taken parts of it: the flags at +0 and +0x110 are what is left of every one seen).
    healthDisplayFrame never sets its ShardTriangle's corners' u and v (the original neither): they are the stack
    residue there, which spawnShardFromTriangle and spawnShards copy into the Shard; on the strict path the last callee
    frame below drawSkinned is vif1PacketAddQword's (the skinned draw's matrix rows), on records requestTextures' saves
    (the VU1 work's calls left out). Read by drawShards alone, into its vertices for drawPrimitiveStrips with the
    shard's texture (0 for every health shard): the strip builder's fixedUv into the staged vertex and the packet
    (nativedef1's watch runs on s-rmpg, s-root, s-dam2-end)."""
    offsets = {BMX_SHARD_CORNERS + BMX_SHARD_CORNER_BYTES * k + lane: None for k in range(3) for lane in (0x10, 0x14)}

    def health_shard_texcoord(address):
        for offset in offsets:
            base = address - offset
            if base & 0xF or base < 0 or base + BMX_SHARD_FLAGS + 4 > len(dump.data):
                continue
            if dump.word(base) == BMX_SHARD_MOVES and dump.word(base + BMX_SHARD_FLAGS) == BMX_HEALTH_SHARD:
                return True
        return False

    return Test(health_shard_texcoord)


def dead_body_rows(dump):
    """Row 3 of a RigidBody's worldInertia or worldInverseInertia left in memory after its level: no walk reaches it, and
    its head or tail may be gone. The rows themselves say what they are: all four w lanes 1, the 3 by 3 symmetric."""
    def still_a_matrix(address):
        if address & 0xF == 0xC or not 0 <= (address & ~0xF) - 0x30 <= len(dump.data) - 0x40:
            return False
        rows = struct.unpack_from('<16f', dump.data, (address & ~0xF) - 0x30)
        if any(rows[r * 4 + 3] != 1.0 for r in range(4)):
            return False
        pairs = ((rows[1], rows[4]), (rows[2], rows[8]), (rows[6], rows[9]))
        return all(abs(x - y) <= 1e-3 * max(abs(x), abs(y), 1e-3) for x, y in pairs) and (rows[0] or rows[5] or rows[10])

    return Test(still_a_matrix)


def node_screen_x(dump):
    """screenPosition.x of every node under g_uiRoot. A node not yet drawn has it as heapTake left it."""
    words = set()
    todo = [dump.word(G_UI_ROOT)]
    seen = set()
    while todo and len(seen) < LONGEST_WALK:
        node = todo.pop()
        if node and node not in seen:
            seen.add(node)
            words.add(node + NODE_SCREEN_POSITION)
            todo += [dump.word(node + NODE_NEXT_SIBLING), dump.word(node + NODE_FIRST_CHILD)]
    return words


# DM2's profiler (ZGE2): g_profilerReady at DM2's address, its bars' pool at +0x08. A ZGE2 bar is AI's ProfileBar
# (zge/debug/profiler.h) without displayMask and _24: cycles at +0x0C, the 64-bit startCount at +0x20.
DM2_PROFILER, PROFILER_BAR_POOL = 0x2A1CD0, 0x08
DM2_BAR_CYCLES, DM2_BAR_START_COUNT = 0x0C, 0x20


def dm2_profile_bar_clock(dump):
    """cycles and startCount (both words) of every bar DM2's profiler pool has handed out: COP0 COUNT as read."""
    words = set()
    for bar in used_pool_records(dump, dump.word(DM2_PROFILER + PROFILER_BAR_POOL)):
        words.update((bar + DM2_BAR_CYCLES, bar + DM2_BAR_START_COUNT, bar + DM2_BAR_START_COUNT + 4))
    return words


# DM2's traffic cars (dm2/level/traffic_cars.cpp): a "Car" level object is a LevelObject (0x14 bytes from
# levelObjectsBegin: its scene object at +0x08, its node at +0x0C, its entity type at +0x10, a pointer to the type's
# {nameHash, init} in g_levelObjectTypesReady, g_levelObjectTypeCount of them) whose type's init is newTrafficCar. The
# car's component is the first record of its node's tag table (ZGE2's Node: the table at +0x114, the name's hash at
# +0x118; objectComponent, tagRecordAt: the records after the 16-byte header and the indices, g_tagIndexWidth[kind]
# bytes each, at the next 16 bytes); its random number is the component's word at 0x18 (kCarTagRandom). The level's
# save (g_levelObjectsSave: its size, then {stateBytes, nameHash, state} records) keeps a copy of each car's component
# at the end of its state (CarSave: the follower's place, then the 0x24-byte component).
DM2_LEVEL_OBJECTS, DM2_LEVEL_OBJECT_BYTES, DM2_LEVEL_OBJECT_SLOTS = 0x2A3B60, 0x14, 0x100
DM2_LEVEL_OBJECT_TYPES, DM2_LEVEL_OBJECT_TYPE_COUNT, DM2_NEW_TRAFFIC_CAR = 0x2A5F60, 0x25BA44, 0x1C4970
DM2_NODE_TAGS, DM2_NODE_NAME_HASH, DM2_TAG_INDEX_WIDTH = 0x114, 0x118, 0x214E68
DM2_LEVEL_OBJECTS_SAVE, DM2_CAR_COMPONENT_BYTES, DM2_CAR_RANDOM = 0x2AAD60, 0x24, 0x18


def dm2_traffic_car_random(dump):
    """The random number of every traffic car's component, and of its copy in the level's save."""
    words, names = set(), set()
    count = min(dump.word(DM2_LEVEL_OBJECT_TYPE_COUNT), 128)
    cars = {DM2_LEVEL_OBJECT_TYPES + 8 * i for i in range(count) if dump.word(DM2_LEVEL_OBJECT_TYPES + 8 * i + 4) == DM2_NEW_TRAFFIC_CAR}
    for i in range(DM2_LEVEL_OBJECT_SLOTS):
        record = DM2_LEVEL_OBJECTS + DM2_LEVEL_OBJECT_BYTES * i
        node = dump.word(record + 0x0C)
        if not dump.word(record + 0x08) or not node or dump.word(record + 0x10) not in cars:
            continue
        names.add(dump.word(node + DM2_NODE_NAME_HASH))
        table = dump.word(node + DM2_NODE_TAGS)
        if table:
            descriptor, elements = dump.word(table + 4), dump.word(table + 8)
            indices = dump.word(DM2_TAG_INDEX_WIDTH + 4 * (descriptor & 0xF)) * elements
            words.add(table + 16 + ((indices + 15) & ~15) + DM2_CAR_RANDOM)
    save = DM2_LEVEL_OBJECTS_SAVE
    end, cursor = save + dump.word(save), save + 4
    while cursor + 8 <= end and len(words) < LONGEST_WALK:
        state_bytes, name = dump.word(cursor), dump.word(cursor + 4)
        if name in names and state_bytes >= DM2_CAR_COMPONENT_BYTES:
            words.add(cursor + 8 + state_bytes - DM2_CAR_COMPONENT_BYTES + DM2_CAR_RANDOM)
        cursor += 8 + state_bytes
    return words


# DM2's particle batches in the frame's packets (tools34): a V4-32 UNPACK to VU1 0x20 or 0xA0 (its NUM qwords, 0 is 256)
# right after an STCYCL 4, 4, the vertices' w of its data: ps2_state_probe.cpp's deadParticleLanes (ZX_PACKET_DEAD_W,
# which run sets for DM2), the stream's words no microprogram reads, where they sit in RAM before the DMA sends them.
DM2_STCYCL_4_4, DM2_PARTICLE_UNPACKS = 0x01000404, (0x6C000020, 0x6C0000A0)


def dm2_particle_lanes(dump):
    """The w of a vertex in a particle batch's UNPACK data, found back from the word: the nearest such UNPACK within
    its longest batch (256 qwords) whose data holds the word as a qword's fourth word."""
    def test(address):
        if address & 3:
            return False
        at = address - 4
        while at >= max(address - 4 - 256 * 16, 4):
            code = dump.word(at)
            if (code & 0xFF00FFFF) in DM2_PARTICLE_UNPACKS and dump.word(at - 4) == DM2_STCYCL_4_4:
                count = (code >> 16 & 0xFF) or 256
                index = (address - at - 4) // 4
                return index < count * 4 and index % 4 == 3
            at -= 4
        return False
    return Test(test)


# DM2's riders' models (tested2): g_riders (16 pointers), a rider's engine object at +0x1218 (Rider::messenger), the
# object's scene at +0x38 (zge sceneOf), the scene's skinned model node at +0x5C (zge::Scene::node), the node's drawn part
# at +0x17C (Node::viewPart), the parts after it by ZgePart::next (+0x30: the rider's shadow model, 87 vertices). A
# skinned part's positions stream (ZgePart +0x04, vertexCount +0x34, skinWeights +0x20) holds the bind positions and
# after them the posed ones (its ZgeStream's bytes 0x10 + 2 x 16 x vertexCount): posePartByBoneWeights writes those,
# their w the VU0 lane a caller-left register's upper half gives (zge/render/shadow.cpp scaleLeftover).
DM2_RIDERS, DM2_RIDER_COUNT, DM2_RIDER_OBJECT, OBJECT_SCENE, DM2_SCENE_NODE, DM2_NODE_VIEW_PART = 0x2F4EA0, 16, 0x1218, 0x38, 0x5C, 0x17C
PART_POSITIONS, PART_SKIN_WEIGHTS, PART_NEXT, PART_VERTEX_COUNT, STREAM_BYTES, STREAM_HEADER = 0x04, 0x20, 0x30, 0x34, 0x04, 0x10


def dm2_rider_posed_w(dump):
    """w of every posed position of each skinned part of each rider's model."""
    words = set()
    for i in range(DM2_RIDER_COUNT):
        rider = dump.word(DM2_RIDERS + 4 * i)
        scene = dump.word(dump.word(rider + DM2_RIDER_OBJECT) + OBJECT_SCENE) if rider else 0
        node = dump.word(scene + DM2_SCENE_NODE) if scene else 0
        for part in dump.chain(dump.word(node + DM2_NODE_VIEW_PART) if node else 0, PART_NEXT):
            stream, count = dump.word(part + PART_POSITIONS), dump.word(part + PART_VERTEX_COUNT) & 0xFFFF
            if not dump.word(part + PART_SKIN_WEIGHTS) or not stream or dump.word(stream + STREAM_BYTES) != STREAM_HEADER + 2 * 16 * count:
                continue
            posed = stream + STREAM_HEADER + 16 * count
            words.update(posed + 16 * k + 0xC for k in range(count))
    return words


# DM2's segment queries' node hits (tested3): QueryHits {segment {start, end}, clipped {start, end}, fraction, node}
# in g_queryHitPool (g_queryState 0x207400 + 0xD844, ZGE2's; zge/scene/query.h QueryHit, zge/memory/pool.h Pool:
# free list at +0x0C). A free record's segment and clipped are written whole (lq/sq) by segmentQueryTestNode right
# after its poolAlloc, before anything reads them, so what a free one holds is read by nothing. The w lanes only, and
# only a word whose exponent is 0 (0 or a denormal), as the proof of the reads while a record is in use needs.
DM2_QUERY_HIT_POOL, POOL_FREE, QUERY_HIT_W_LANES = 0x207400 + 0xD844, 0x0C, (0x0C, 0x1C, 0x2C, 0x3C)
FLOAT_EXPONENT = 0x7F800000


def dm2_free_query_hit_w(dump):
    """w of segment.start, segment.end, clipped.start and clipped.end of every free QueryHit record, when 0 or a
    denormal."""
    words = set()
    pool = dump.word(DM2_QUERY_HIT_POOL)
    if not pool:
        return words
    for link in dump.chain(dump.word(pool + POOL_FREE), 0):
        for lane in QUERY_HIT_W_LANES:
            address = link + POOL_LINK_BYTES + lane
            if not dump.word(address) & FLOAT_EXPONENT:
                words.add(address)
    return words


# BMX XXX's sphere queries' node hits (bmxw1): QueryHits {segment {start, end}, clipped {start, end}, fraction, node} in
# g_queryHitPool (g_queryState 0x252B00 + 0x48; zge/scene/query.h QueryHit, zge/memory/pool.h Pool: used list +0x00,
# free list +0x0C, a record 0x10 past its link, 0x60 apart). clipped.start.w and clipped.end.w are what clipSegmentToPlane's
# cut point takes from the stack (vec3Lerp writes x, y and z; its frame's w is dead stack another call left), so they
# follow the stack residue of whichever functions ran there; the proof that nothing reads them is the allow line's.
BMX_QUERY_HIT_POOL, QUERY_HIT_CLIPPED_W = 0x252B00 + 0x48, (0x2C, 0x3C)


def bmx_query_hit_clipped_w(dump):
    """w of clipped.start and clipped.end of every QueryHit record of BMX XXX's hit pool, in use or free."""
    words = set()
    pool = dump.word(BMX_QUERY_HIT_POOL) & 0x1FFFFFFF
    if not pool:
        return words
    for head in (POOL_USED, POOL_FREE):
        for link in dump.chain(dump.word(pool + head) & 0x1FFFFFFF, 0):
            words.update((link & 0x1FFFFFFF) + POOL_LINK_BYTES + lane for lane in QUERY_HIT_CLIPPED_W)
    return words


# DM2's rigid bodies (tested3): dynamicsObjects (0x2C8BE0, g_dynamicsObjectCount 0x25BB44) -> the record's dynamics
# (+0x0C, dm2/level/dynamics_objects.h DynamicsRecord) -> its bodies (+0x0C, zge::Dynamics' DM2 offset), each the next's
# by RigidBody::next (DM2 +0x408). The body's matrices are AI's: worldInertia +0x90, worldInverseInertia +0x120,
# lastInverseInertia +0x220.
DM2_DYNAMICS_OBJECTS, DM2_DYNAMICS_OBJECT_COUNT, DM2_RECORD_DYNAMICS, DM2_DYNAMICS_BODIES, DM2_BODY_NEXT = 0x2C8BE0, 0x25BB44, 0x0C, 0x0C, 0x408
DM2_BODY_ROW3S = (0xC0, 0x150, 0x250)


def dm2_bodies(dump):
    for i in range(min(dump.word(DM2_DYNAMICS_OBJECT_COUNT), 256)):
        record = dump.word(DM2_DYNAMICS_OBJECTS + 4 * i)
        dynamics = dump.word(record + DM2_RECORD_DYNAMICS) if record else 0
        yield from dump.chain(dump.word(dynamics + DM2_DYNAMICS_BODIES) if dynamics else 0, DM2_BODY_NEXT)


def dm2_body_inertia_rows(dump):
    """x, y, z of row 3 of worldInertia, worldInverseInertia and lastInverseInertia of every body of every dynamics
    object."""
    words = set()
    for body in dm2_bodies(dump):
        for row in DM2_BODY_ROW3S:
            words.update(body + row + lane for lane in (0, 4, 8))
    return words


# A 2D batch's S and T in the frame's packets (tested2): ps2_state_probe.cpp's UniformTexcoords at the words' place in
# RAM. A batch is a DMA CNT whose VIF codes are STCYCL 4, 4 and a V4-32 UNPACK whose data starts with a PACKED GIF tag
# of ST, RGBAQ, XYZ2 (0x512, NREG 3); its texture is the A+D packet the DMA REF just before it sends (STCYCL 4, 4 and a
# V4-32 UNPACK in the REF's own upper words): TEX0_1 and CLAMP_1 there. Uniform as the stream's rule has it: zgui's
# default 8x8 T4 (newUiDefaultMaterial: every texel index 0, every palette entry white), or a 1x1 texture wrapped by
# REPEAT or CLAMP: every sample is that one texel whatever S and T are.
STCYCL_4_4, UNPACK_V4_32 = 0x01000404, 0x6C000000
GIF_ST_RGBAQ_XYZ2, GIF_A_D = 0x512, 0xE
GS_TEX0_1, GS_CLAMP_1, GS_PSM_T4 = 0x06, 0x08, 0x14


def packet_2d_uniform_texcoords(dump):
    """S or T of a vertex in a 2D batch whose texture's every sample is one colour (see above)."""
    def unpack_at(at):
        """(data address, qwords) of a V4-32 UNPACK whose code word is at `at`, after STCYCL 4, 4; else None."""
        code = dump.word(at)
        if (code & 0xFF000000) != UNPACK_V4_32 or dump.word(at - 4) != STCYCL_4_4:
            return None
        return at + 4, (code >> 16 & 0xFF) or 256

    def uniform_texture(packet):
        """The A+D packet at `packet` (its GIF tag, then data, register pairs) sets a uniform TEX0_1."""
        tag = [dump.word(packet + 4 * i) for i in range(4)]
        if tag[2] != GIF_A_D or tag[1] >> 28 != 1 or tag[3] != 0:
            return False
        tex0 = clamp = None
        for i in range(tag[0] & 0x7FFF):
            low, high, register = dump.word(packet + 16 + 16 * i), dump.word(packet + 20 + 16 * i), dump.word(packet + 24 + 16 * i) & 0xFF
            if register == GS_TEX0_1:
                tex0 = (low, high)
            elif register == GS_CLAMP_1:
                clamp = low
        if tex0 is None:
            return False
        low, high = tex0
        width, height = low >> 26 & 0xF, (low >> 30 & 3) | (high & 3) << 2
        if (low >> 20 & 0x3F) == GS_PSM_T4 and width == 3 and height == 3:
            return True
        wraps_inside = clamp is None or ((clamp & 3) < 2 and (clamp >> 2 & 3) < 2)  # (CLAMP_1 0 at reset: REPEAT)
        return width == 0 and height == 0 and wraps_inside

    def test(address):
        if address & 3:
            return False
        at = address - 4
        while at >= max(address - 4 - 256 * 16, 0x14):
            found = unpack_at(at)
            if found:
                data, qwords = found
                if dump.word(data + 8) != GIF_ST_RGBAQ_XYZ2 or dump.word(data + 4) >> 26 != 3 << 2 or dump.word(data + 12):
                    return False
                index = (address - data - 16) // 4
                if not 0 <= index < min((dump.word(data) & 0x7FFF) * 12, qwords * 4 - 4) or index % 12 > 1:
                    return False
                # The batch's DMA CNT (its data starts with the STCYCL and the UNPACK) and the REF before it.
                tag = at - 4 - 16
                if tag & 0xF or dump.word(tag) >> 28 & 7 != 1:
                    return False
                ref = tag - 16
                if dump.word(ref) >> 28 & 7 != 3 or not unpack_at(ref + 12):
                    return False
                return uniform_texture(dump.word(ref + 4))
            at -= 4
        return False
    return Test(test)


# The bits of a GIF packet's PACKED data the GS does not read, where a passthrough microprogram sends the packet (tested2;
# ps2_state_probe.cpp's Passthrough, ZX_PACKET_PASSTHROUGH, at the words' place in RAM): a TOPS-relative V4-32 UNPACK
# (after STCYCL 4, 4) whose data starts with a PACKED GIF tag, an MSCAL of a passthrough program (DM2's 0x1C: XTOP vi1,
# XGKICK vi1, end) after it before any other MSCAL, and the word in one of the tag's register qwords: its bits that
# register does not read (the GS manual's PACKED formats, zx-vu1check's packedMask).
PASSTHROUGH_PROGRAMS = {'dm2': (0x1C,)}
PACKED_MASKS = {0x1: (0xFF, 0xFF, 0xFF, 0xFF), 0x2: (0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0), 0x3: (0x3FFF, 0x3FFF, 0, 0),
                0x4: (0xFFFF, 0xFFFF, 0x0FFFFFF0, 0x8FF0), 0xC: (0xFFFF, 0xFFFF, 0x0FFFFFF0, 0x8FF0),
                0x5: (0xFFFF, 0xFFFF, 0xFFFFFFFF, 0x8000), 0xD: (0xFFFF, 0xFFFF, 0xFFFFFFFF, 0x8000),
                0xA: (0, 0, 0, 0xFF0), 0xE: (0xFFFFFFFF, 0xFFFFFFFF, 0xFF, 0), 0xF: (0, 0, 0, 0)}


class Bits(dict):
    """A rule of bits: `in` works a word out (its unread bits, or not the rule's), `[]` gives the bits that may differ."""

    def __init__(self, unread):
        super().__init__()
        self.unread = unread

    def __contains__(self, address):
        if not dict.__contains__(self, address):
            dict.__setitem__(self, address, self.unread(address))
        return dict.__getitem__(self, address) is not None

    def __getitem__(self, address):
        return dict.__getitem__(self, address) if address in self else 0


def passthrough_unread_bits(game):
    programs = PASSTHROUGH_PROGRAMS.get(game, ())

    def rule(dump):
        def unread(address):
            if address & 3 or not programs:
                return None
            at = address - 4
            while at >= max(address - 4 - 256 * 16, 4):
                code = dump.word(at)
                if (code & 0xFF008000) == 0x6C008000 and dump.word(at - 4) == STCYCL_4_4:
                    break
                at -= 4
            else:
                return None
            count = (dump.word(at) >> 16 & 0xFF) or 256
            data = at + 4
            index = (address - data) // 4
            if index >= count * 4:
                return None
            tag = [dump.word(data + 4 * i) for i in range(4)]
            low, regs = tag[0] | tag[1] << 32, tag[2] | tag[3] << 32
            nloop, flg, nreg = low & 0x7FFF, low >> 58 & 3, (low >> 60 & 15) or 16
            qword = index // 4 - 1
            if flg != 0 or qword < 0 or qword >= min(nloop * nreg, count - 1):
                return None
            # The next MSCAL after the UNPACK's data: a passthrough program's.
            after = data + count * 16
            for x in range(after, after + 4 * 1024, 4):
                if dump.word(x) >> 24 == 0x14:
                    if dump.word(x) & 0xFFFF not in programs:
                        return None
                    mask = PACKED_MASKS.get(regs >> (4 * (qword % nreg)) & 15, (0xFFFFFFFF,) * 4)[index % 4]
                    return (~mask & 0xFFFFFFFF) or None
            return None
        return Bits(unread)
    return rule


class Test:
    """A rule that looks at the word's own surroundings: `in` calls it."""

    def __init__(self, test):
        self.test = test

    def __contains__(self, address):
        return self.test(address)


# The renderer (zge/render/renderer.h) per game: its address, the two frame packet buffers' words, the mesh packet
# (sceVif1Packet, 0x18 bytes), the texture request's marker (a pointer into the mesh packet), the statistics' fewest
# free bytes (DM2: none); the frame fix-ups' two lists (g_fixupLists: 32 FrameFixup records of 12 bytes each, the
# word to patch at +4: a pointer into the frame packet; DM2: none found yet).
FRAME_PACKET_BUFFER_BYTES = 0x20000
RENDERERS = {'ai': (0x27A9B0, 0x04, 0x20, 0x80, 0xAC, 0x2791D8), 'dm2': (0x25CDA0, 0x08, 0x20, 0x5C, None, None),
             'bmx': (0x2A40E0, 0x04, 0x10, 0x74, 0xA0, 0x2A26D8)}
FIXUP_RECORDS, FIXUP_BYTES, FIXUP_WORD = 32, 12, 4


def frame_packets(game):
    """render_path = records (z-axis zge/host/class_draws.cpp): a draw made natively from its record leaves its VU1
    work out of the view's mesh packet (its texture request, VU1 state and a NOP anchor stay), so the frame packets'
    buffers hold other words at other places for the same frames. The words: both frame packet buffers whole (as the
    renderer's frameBuffers name them in the dump: what the packet library writes and the DMA reads), the mesh
    packet's cursor words, the texture request's marker, the statistics' fewest free packet bytes, and the word each
    frame fix-up patches (pointers into the packets). Which code reads them is the watch run's
    (ZX_LANE_WATCH over the buffers: the packet library, the frame fix-ups and the DMA only, native1)."""
    base, buffers, mesh, marker, least, fixups = RENDERERS[game]

    def rule(dump):
        spans = []
        for i in range(2):
            start = dump.word(base + buffers + 4 * i) & 0x1FFFFFFF
            if start:
                spans.append((start, start + FRAME_PACKET_BUFFER_BYTES))
        words = set(range(base + mesh, base + mesh + 0x18, 4))
        words.add(base + marker)
        if least is not None:
            words.add(base + least)
        if fixups is not None:
            for i in range(2):
                records = dump.word(fixups + 4 * i) & 0x1FFFFFFF
                if records:
                    words.update(records + FIXUP_BYTES * k + FIXUP_WORD for k in range(FIXUP_RECORDS))
        return Test(lambda address: address in words or any(lo <= address < hi for lo, hi in spans))
    return rule


# DM2's screen primitives (PrimitivePacket, zge/render/ps2/primitive_packets.cpp, DM2's g_screenPrimitives): their DMA
# buffers, a PointerArray of them per buffer set (+0x28, +0x2C; ZGE2's array: items, count), each bufferBytes (+0x24).
DM2_SCREEN_PRIMITIVES = 0x25CC68


def dm2_screen_primitive_buffers(dump):
    """render_path = records in DM2: a sprite's S and T that its staged vertex leaves unset, copied into the screen
    primitives' DMA buffers by sceVif1PkAddGsDataN: they hold what earlier code left there (in the defaults the mesh
    packet's address, from the class draws' encoder calls the records path leaves out), denormal junk either way. The
    packet library writes those buffers and only the DMA reads them (native1's watch run on fr-woodward)."""
    size = dump.word(DM2_SCREEN_PRIMITIVES + 0x24)
    spans = []
    for k in range(2):
        array = dump.word(DM2_SCREEN_PRIMITIVES + 0x28 + 4 * k) & 0x1FFFFFFF
        if not array:
            continue
        items, count = dump.word(array) & 0x1FFFFFFF, dump.word(array + 4)
        for i in range(min(count, 16)):
            start = dump.word(items + 4 * i) & 0x1FFFFFFF
            if start:
                spans.append((start, start + size))
    return Test(lambda address: any(lo <= address < hi for lo, hi in spans))


OWNERS = {'UiAsset.textureFlags': ui_texture_flags, 'UiAsset.freedTextureFlags': freed_ui_records,
          'RigidBody.inertiaRow3': body_inertia_rows, 'RigidBody.deadInertiaRow3': dead_body_rows,
          'RigidBody.wLanes': body_w_lanes, 'Node.matrixW': node_w_lanes,
          'Player.bobScaleW': bob_scale_w, 'SplashRing.pointW': splash_ring_w, 'HeapBlock.pastAllocation': past_allocation, 'Node.screenPositionX': node_screen_x,
          'Dm2ProfileBar.clock': dm2_profile_bar_clock, 'Dm2TrafficCar.random': dm2_traffic_car_random,
          'SaveMessageQueue.freeSlot': save_queue_free_slots,
          'Dm2Packet.particleW': dm2_particle_lanes, 'Packet2d.uniformTexcoords': packet_2d_uniform_texcoords,
          'Dm2Rider.posedW': dm2_rider_posed_w, 'Dm2QueryHit.freeW': dm2_free_query_hit_w,
          'BmxQueryHit.clippedW': bmx_query_hit_clipped_w,
          'Dm2RigidBody.inertiaRow3': dm2_body_inertia_rows,
          'Dm2Packet.passthroughUnread': passthrough_unread_bits('dm2'),
          'Renderer.framePackets': frame_packets('ai'), 'Dm2Renderer.framePackets': frame_packets('dm2'),
          'BmxRenderer.framePackets': frame_packets('bmx'), 'BmxShard.healthTexcoords': bmx_health_shard_texcoords, 'Dm2ScreenPrimitives.buffers': dm2_screen_primitive_buffers}


class Frame:
    """A `frame` line: one function's stack frame."""

    def __init__(self, line):
        words = line.split()
        self.name = words[1]
        start, _, size = words[2].partition('+')
        self.start, self.end = int(start, 0), int(start, 0) + int(size, 0)
        self.size = self.ra = self.stack = None
        self.last = False
        self.saves, self.unused, self.string = (), (), None
        for word in words[3:]:
            key, _, value = word.partition('=')
            if word == 'last':
                self.last = True
            elif key in ('size', 'ra', 'stack'):
                setattr(self, key, int(value, 0))
            elif key in ('saves', 'unused'):
                setattr(self, key, tuple(int(v, 0) for v in value.split(',')))
            elif key == 'string':
                at, _, bytes_ = value.partition('+')
                self.string = (int(at, 0), int(bytes_, 0))
            else:
                break


def frames_of(thread, dump, table):
    """The frames (Frame, its sp) from a thread's sp to the top of its stack: the longest chain of saved return
    addresses through `table` that gets there (a stale return address can skip frames, never add one), [] if none
    does or two as long do."""
    tops = (thread.stack, thread.stack + thread.size)  # (main's record says where its stack ends)
    table = [frame for frame in table if frame.stack == thread.stack]

    def walk(frame, sp):
        if frame.last:
            return [(frame, sp)] if sp + frame.size in tops else None
        back = dump.word(sp + frame.ra)
        for caller in table:
            if caller.start <= back < caller.end:
                rest = walk(caller, sp + frame.size)
                return rest and [(frame, sp)] + rest
        return None

    chains = sorted((c for c in (walk(frame, thread.sp) for frame in table) if c), key=len, reverse=True)
    return chains[0] if chains and (len(chains) == 1 or len(chains[0]) > len(chains[1])) else []


class FrameWords:
    """The allowed words of the live frames of a dump pair, walked in the `off` dump."""

    def __init__(self, dumps, table):
        self.plain, self.strings = set(), []
        off, on = dumps
        if off.threads is None or on.threads is None or not table:
            return
        same = {(t.id, t.sp) for t in on.threads}
        for thread in off.threads:
            if (thread.id, thread.sp) not in same:
                continue  # (frames at another sp are not the same frames)
            for frame, sp in frames_of(thread, off, table):
                for slot in frame.saves:
                    self.plain.update((sp + slot, sp + slot + 4))
                self.plain.update(sp + word for word in frame.unused)
                if frame.string:
                    self.strings.append((sp + frame.string[0], frame.string[1]))

    def holds(self, address, dumps):
        if address in self.plain:
            return True
        return any(at <= address < at + size and all(address >= d.data.find(b'\0', at, at + size) >= 0 for d in dumps)
                   for at, size in self.strings)


class Allowed:
    """One line of the allow list; see the module's text."""

    def __init__(self, line, folder=''):
        spec, _, rest = line.partition(' ')
        self.owner = None
        self.lanes = self.input = None
        self.streams = ()
        if spec.startswith('stream='):
            # What rendering is handed (compare_packets): a stream that differs by design between the two runs (a
            # render path that hands the GS other work for the same frames: render_path = records); its vsyncs are
            # counted apart under the reason. The RAM behind it is compared as ever (owner rules say which words).
            self.streams = tuple(spec[len('stream='):].split(','))
            if not set(self.streams) <= set(PACKET_STREAMS):
                raise SystemExit('ab_allow: no stream %s' % spec)
            spec = '0+0'
        if spec.startswith('lanes='):
            # wlane.py poke's words (tooling/checks/wlane.py): the RAM words a leftover lane moves between two forced
            # values on one input, at each dump, with nothing observable moving and every reader followed to a
            # discard. `tTICK 0xADDR ...` lines; the dump's label and the word must both match.
            path = spec[len('lanes='):]
            path = path if os.path.isabs(path) else os.path.join(folder, path)
            self.lanes = set()
            for text in open(path):
                m = re.match(r'^(t\d+) 0x([0-9A-Fa-f]+)', text)
                if m:
                    self.lanes.add((m.group(1), int(m.group(2), 16)))
            spec = '0+0x100000000'
        if spec.startswith('owner='):
            self.owner = spec[len('owner='):]
            if self.owner != 'frames' and self.owner not in OWNERS:
                raise SystemExit('ab_allow: no owner rule %s' % self.owner)
            spec = '0+0x100000000'
        addr, _, size = spec.partition('+')
        self.start, self.size = int(addr, 0), int(size or '4', 0)
        self.within = self.every = self.stack_of = self.bits = None
        self.at = ()
        self.stack = False
        words = rest.split()
        while words and (words[0] == 'stack' or re.fullmatch(r'(within|every|at|stack-of|bits|input)=\S+', words[0])):
            key, _, value = words.pop(0).partition('=')
            if key == 'input':
                self.input = value
            elif key == 'within':
                self.within = int(value, 0)
            elif key == 'every':
                self.every = int(value, 0)
            elif key == 'at':
                self.at = tuple(int(v, 0) for v in value.split(','))
            elif key == 'stack-of':
                self.stack_of = int(value, 0)
            elif key == 'bits':
                self.bits = int(value, 0)
            else:
                self.stack = True
        self.reason = ' '.join(words) or 'allowed'

    def covers(self, address, va, vb, threads, dumps=(), frames=None, label=None):
        if not self.start <= address < self.start + self.size:
            return False
        if self.input is not None and self.input != CURRENT_INPUT[0]:
            return False
        if self.lanes is not None and (label, address) not in self.lanes:
            return False
        if self.owner == 'frames' and not (frames and frames.holds(address, dumps)):
            return False
        if self.owner in OWNERS:
            for dump in dumps:
                owned = dump.owned(self.owner)
                if address not in owned:
                    return False
                if isinstance(owned, dict) and (va ^ vb) & ~owned[address] & 0xFFFFFFFF:
                    return False
        if self.every and (address - self.start) % self.every not in self.at:
            return False
        if self.bits is not None and (va ^ vb) & ~self.bits & 0xFFFFFFFF:
            return False  # (only those bits of the word may differ)
        if self.within is not None and min((va - vb) & 0xFFFFFFFF, (vb - va) & 0xFFFFFFFF) > self.within:
            return False
        if self.stack or self.stack_of is not None:
            if None in threads:
                return False
            if self.stack:  # no thread is on it
                return not any(self.start <= t.sp < self.start + self.size for side in threads for t in side)
            return all(any(t.id == self.stack_of and address < t.sp for t in side) for side in threads)
        return True


def load_allow(paths):
    allow, frames = [], []
    for path in (paths.split(',') if paths else ()):  # (several files: the gate's list and a setting's own)
        for line in open(path):
            line = line.split('#')[0].strip()
            if line.startswith('frame '):
                frames.append(Frame(line))
            elif line:
                allow.append(Allowed(line, os.path.dirname(os.path.abspath(path))))
    return allow, frames


# The input of the runs `compare` is looking at (their run.txt), for allow lines that hold on one input (input=NAME).
CURRENT_INPUT = [None]


# (DM2's main thread is made with the stack's top as its stack word: its stack is the size below that.)
STACK_WORD_IS_TOP = [False]


class Thread:
    def __init__(self, parts):
        self.id, self.status = int(parts[0]), int(parts[1])
        self.stack, self.size, self.sp = (int(p, 0) for p in parts[2:5])
        if STACK_WORD_IS_TOP[0] and self.stack - self.size <= self.sp < self.stack:
            self.stack -= self.size


def threads_of(dump):
    """The guest threads at a RAM dump (its .sp file), None if it has none."""
    path = dump[:-4] + '.sp'
    if not os.path.exists(path):
        return None
    return [Thread(line.split()) for line in open(path) if line.strip()]


def below_sp(address, threads):
    """Below the sp of the thread whose stack holds the word, in both runs."""
    def dead(side):
        return side is not None and any(t.stack and t.stack <= address < t.stack + t.size and address < t.sp for t in side)
    return all(dead(side) for side in threads)


def region_of(address):
    for start, end, name in RAM_REGIONS:
        if start <= address < end:
            return name
    return 'above 32 MB'


def compare_dump(path_a, path_b):
    a = open(path_a, 'rb').read()
    b = open(path_b, 'rb').read()
    words = []
    n = min(len(a), len(b))
    block = 1 << 14
    for at in range(0, n, block):
        if a[at:at + block] == b[at:at + block]:
            continue
        wa = array.array('I', a[at:at + block])
        wb = array.array('I', b[at:at + block])
        for i in range(min(len(wa), len(wb))):
            if wa[i] != wb[i]:
                words.append((at + i * 4, wa[i], wb[i]))
    return words, a, b


def compare_ram(a_dir, b_dir, report, problems, allowed_lists, out, listed):
    allow, frame_table = allowed_lists

    def dumps(directory):
        return {os.path.basename(p)[5:-4]: p for p in glob.glob(os.path.join(directory, 'dump-*.ram'))}

    a, b = dumps(a_dir), dumps(b_dir)
    common = sorted(set(a) & set(b), key=lambda s: (len(s), s))
    if not common:
        report('RAM: NOT COMPARED: no dump in both runs (%d and %d dumps)' % (len(a), len(b)))
        problems.append('no RAM dumps')
        return
    if set(a) ^ set(b):
        report('RAM: NOT COMPARED: dumps in one run only: %s' % sorted(set(a) ^ set(b)))
    total = 0
    persistent = collections.Counter()
    sink = open(out, 'w') if out else None
    for label in common:
        words, data_a, data_b = compare_dump(a[label], b[label])
        size_a, size_b = len(data_a), len(data_b)
        total += len(words)
        allowed = collections.Counter()
        kept = []
        threads = (threads_of(a[label]), threads_of(b[label]))
        pair = (Dump(data_a, threads[0]), Dump(data_b, threads[1]))
        frames = FrameWords(pair, frame_table)
        for address, va, vb in words:
            if below_sp(address, threads):
                allowed['dead stack: below its thread\'s sp'] += 1
                continue
            for line in allow:
                if line.covers(address, va, vb, threads, pair, frames, label):
                    allowed[line.reason] += 1
                    break
            else:
                kept.append((address, va, vb))
        regions = collections.Counter(region_of(w[0]) for w in kept)
        pages = collections.Counter(w[0] >> 16 for w in kept)
        report('RAM %s: %s of %s words differ%s; %s; densest 64 KB: %s' % (
            label, format(len(kept), ','), format(size_a // 4, ','),
            ' (and %s allowed: %s)' % (format(sum(allowed.values()), ','), ', '.join('%d %s' % (n, r) for r, n in allowed.most_common())) if allowed else '',
            ', '.join('%s %d' % (r, n) for r, n in regions.most_common()) or 'none',
            ', '.join('0x%X0000 %d' % kv for kv in pages.most_common(4)) or 'none'))
        if size_a != size_b:
            report('  NOT COMPARED: the dumps differ in size (%d and %d)' % (size_a, size_b))
            problems.append('dump sizes')
        for address, va, vb in kept[:listed]:
            report('    0x%08X  A %08X  B %08X' % (address, va, vb))
        if len(kept) > listed:
            report('    ... %s more%s' % (format(len(kept) - listed, ','), ' in ' + out if out else ' (--out FILE for every word)'))
        if sink:
            for address, va, vb in kept:
                sink.write('%s 0x%08X %08X %08X %s\n' % (label, address, va, vb, region_of(address).replace(' ', '_')))
        if kept:
            problems.append('RAM %s: %d words' % (label, len(kept)))
        for address, _, _ in kept:
            persistent[address] += 1
    if sink:
        sink.close()
    every = [k for k, v in persistent.items() if v == len(common)]
    if len(common) > 1 and persistent:
        report('RAM: %s words differ in every dump, %s in only some' % (format(len(every), ','), format(len(persistent) - len(every), ',')))


def whole_lines(path):
    """A log's lines but a cut last one. A run ends by a kill at STOP_TICK, so these files end where the
    last full stdio buffer did (their size is a multiple of 4096): the line it cuts has lost its last
    fields, or part of a hash, and a cut hash is not a value to compare."""
    with open(path, errors='replace') as f:
        for line in f:
            if line.endswith('\n'):
                yield line


def scratchpad_hashes(directory):
    path = os.path.join(directory, 'scratchpad.txt')
    hashes = {}
    if os.path.exists(path):
        for line in whole_lines(path):
            parts = line.split()
            if len(parts) == 4 and parts[0] == 'H':
                hashes[int(parts[1])] = parts[3]
    return hashes


def compare_scratchpad(a_dir, b_dir, report, problems):
    a, b = scratchpad_hashes(a_dir), scratchpad_hashes(b_dir)
    common = sorted(set(a) & set(b))
    if not common:
        report('scratchpad: NOT COMPARED: no hashes in both runs')
        return
    differ = [t for t in common if a[t] != b[t]]
    report('scratchpad: %s vsyncs hashed in both, %s differ%s (16 KB as one hash: no words)' % (
        format(len(common), ','), format(len(differ), ','), ', first at tick %d' % differ[0] if differ else ''))
    if differ:
        problems.append('scratchpad at %d vsyncs' % len(differ))


def packet_lines(directory):
    path = os.path.join(directory, 'packets.txt')
    lines = {}
    if os.path.exists(path):
        for line in whole_lines(path):
            parts = line.split()
            if len(parts) in (12, 13) and parts[0] == 'P':  # (13: the vif1 live hash)
                lines[int(parts[1])] = parts[2:]
    return lines


def compare_packets(a_dir, b_dir, report, problems, allow=()):
    a, b = packet_lines(a_dir), packet_lines(b_dir)
    common = sorted(set(a) & set(b))
    if not common:
        report('rendering: NOT COMPARED: no packet hashes in both runs')
        return
    total = 0
    for i, name in enumerate(PACKET_STREAMS):
        total += sum(int(a[t][2 * i]) for t in common)
        differ = [t for t in common if a[t][2 * i:2 * i + 2] != b[t][2 * i:2 * i + 2]]
        sized = sum(1 for t in differ if a[t][2 * i] != b[t][2 * i])
        report('rendering %-4s: %s vsyncs, %s bytes, %s differ%s' % (
            name, format(len(common), ','), format(sum(int(a[t][2 * i]) for t in common), ','), format(len(differ), ','),
            ' (%s in size), ticks %d to %d' % (format(sized, ','), differ[0], differ[-1]) if differ else ''))
        # Two rows' w, stack residue the original leaves, that no microprogram reads: VU1 0x3F3's
        # (camera->forward.w) and qword 9's of the 14 quadwords to VU1 1 (zx-vu1check --reads and --poke;
        # ps2_state_probe.cpp's kDeadWords). Counted apart.
        dead = [t for t in differ if name == 'vif1' and len(a[t]) > 10 and len(b[t]) > 10 and a[t][10] == b[t][10]]
        if dead:
            report('rendering vif1: %s of those only in words nothing reads (kDeadWords)' % format(len(dead), ','))
        by_design = [line for line in allow if name in line.streams and (line.input is None or line.input == CURRENT_INPUT[0])]
        if by_design and len(differ) > len(dead):
            report('rendering %s: %s vsyncs allowed: %s' % (name, format(len(differ) - len(dead), ','), by_design[0].reason))
        elif len(differ) > len(dead):  # (gs too: the probe drains the GS worker before it hashes the registers, tools30)
            problems.append('%s stream at %d vsyncs' % (name, len(differ) - len(dead)))
    for i, name in ((8, 'VU0 vf'), (9, 'VU0 ACC, Q, VI, flags')):
        differ = [t for t in common if a[t][i] != b[t][i]]
        report('%s at the vsync: %s of %s differ%s' % (name, format(len(differ), ','), format(len(common), ','),
                                                       ', ticks %d to %d' % (differ[0], differ[-1]) if differ else ''))
        if differ:
            problems.append('%s at %d vsyncs' % (name, len(differ)))


def script_of(args):
    """A pad script of a ZX_PAD_TRACE file: each button's presses and each stick's moves."""
    held = {}
    items = []
    last = 0
    sticks = {'RX': 0x80, 'RY': 0x80, 'LX': 0x80, 'LY': 0x80}
    stick_from = {}
    with open(args.trace) as f:
        for line in f:
            parts = line.split()
            if len(parts) != 6:
                continue
            tick = int(parts[0])
            last = tick
            raw = int(parts[1], 16)
            mask = ~(((raw & 0xFF) << 8) | (raw >> 8)) & 0xFFFF
            for bit, name in PAD_BUTTONS.items():
                if mask & bit and bit not in held:
                    held[bit] = tick
                elif not mask & bit and bit in held:
                    start = held.pop(bit)
                    items.append((start, '%dv:%s:%dv' % (start, name, tick - start)))
            for name, text in zip(('RX', 'RY', 'LX', 'LY'), parts[2:]):
                value = int(text, 16)
                if value != sticks[name]:
                    if sticks[name] != 0x80:
                        start = stick_from[name]
                        items.append((start, '%dv:%s=%.4f:%dv' % (start, name, (sticks[name] - 128) / 127.0, tick - start)))
                    sticks[name] = value
                    stick_from[name] = tick
    for bit, start in held.items():
        items.append((start, '%dv:%s:%dv' % (start, PAD_BUTTONS[bit], last + 60 - start)))
    items.sort()
    with open(args.out, 'w') as f:
        for pair in args.env:
            f.write('#env %s\n' % pair)
        if args.pokes:
            pokes = [line.split() for line in open(args.pokes) if line.strip()]
            f.write('#env ZX_POKE_SCRIPT=%s\n' % ','.join('%s:%s=%s' % tuple(p) for p in pokes))
        f.write(','.join(text for _, text in items) + '\n')
    print('%d presses to tick %d in %s' % (len(items), last, args.out))


def packets(args):
    """The first differing word of a tick's stream, what is around it, and how many differ."""
    paths = [os.path.join(d, 'packets.txt.t%d.%s' % (args.tick, args.stream)) for d in (args.a, args.b)]
    a, b = (open(p, 'rb').read() for p in paths)
    print('%s bytes and %s bytes' % (format(len(a), ','), format(len(b), ',')))
    n = min(len(a), len(b)) // 4
    wa = struct.unpack('<%dI' % n, a[:n * 4])
    wb = struct.unpack('<%dI' % n, b[:n * 4])
    differ = [i for i in range(n) if wa[i] != wb[i]]
    print('%s of %s words differ' % (format(len(differ), ','), format(n, ',')))
    for i in differ[:args.list]:
        print('  +0x%X  A %08X (%g)  B %08X (%g)' % (i * 4, wa[i], as_float('%x' % wa[i]), wb[i], as_float('%x' % wb[i])))
    if differ:
        at = differ[0] & ~3
        for i in range(max(at - 16, 0), min(at + 16, n), 4):
            print('  +0x%06X  %s' % (i * 4, ' '.join('%08X' % w for w in wa[i:i + 4])))
    return 1 if differ or len(a) != len(b) else 0


def compare(args):
    problems = []
    lines = []

    def report(text):
        print(text)
        lines.append(text)

    clocks = []
    for d in (args.a, args.b):
        info = os.path.join(d, 'run.txt')
        if os.path.exists(info):
            text = open(info).read()
            report('%s: %s' % (d, ' | '.join(text.split('\n')[:3])))
            clock = re.search(r'^clock (\d+)$', text, re.M)
            clocks.append(int(clock.group(1)) if clock else 1)
            given = re.search(r'^input (\S+)$', text, re.M)
            if given:
                CURRENT_INPUT[0] = given.group(1)
            game = re.search(r'^game (\w+)$', text, re.M)
            if game:
                RAM_REGIONS[:] = GAMES[game.group(1)]['regions']
                STACK_WORD_IS_TOP[0] = True
    if len(clocks) == 2 and clocks[0] != clocks[1]:
        report('NOT COMPARABLE: the runs are at different clocks (%d and %d)' % tuple(clocks))
        problems.append('different clocks')
    if not args.dumps_only:
        compare_state(args.a, args.b, report, problems)
        compare_page(args.a, args.b, report, problems)
    if not args.state_only:
        compare_scratchpad(args.a, args.b, report, problems)
        compare_packets(args.a, args.b, report, problems, load_allow(args.allow)[0])
        if getattr(args, 'ram_information', False):
            # (an input whose RAM words are not proven either way yet: reported, not counted; GAMES' ram_information)
            ram = []
            compare_ram(args.a, args.b, report, ram, load_allow(args.allow), args.out, args.list)
            if ram:
                report('RAM as information, not counted: ' + '; '.join(ram))
        else:
            compare_ram(args.a, args.b, report, problems, load_allow(args.allow), args.out, args.list)
    report('NOT HELD by either run, so not compared: ' + NOT_HELD)
    report('RESULT: ' + ('identical in everything compared' if not problems else '%d differences: %s' % (
        len(problems), '; '.join(problems[:12]) + (' ...' if len(problems) > 12 else ''))))
    return 0 if not problems else 1


def free_memory():
    text = subprocess.run(['memory_pressure', '-Q'], capture_output=True, text=True).stdout
    found = re.search(r'percentage: (\d+)%', text)
    return int(found.group(1)) if found else 100


# A run's peak memory, for the room a start needs where a cgroup caps it (the Linux worker: rcheck.slice's MemoryMax 15 GB; tools30,
# after the cap killed three valgrind pairs started together beside a checker trial, 11-14 14:42). Measured on the Linux worker
# (11-15, the coll runner's DM2 plant run under memcheck --track-origins=yes): VmHWM 3,236 MB in its first minute,
# reading the 2.2 GB binary's debug information, then 2.1-2.2 GB a side as residue1 saw; a native runner's own share of
# the b109 gate's 1,278 MB peak for 8 runners (memplan.py) is under 200 MB, taken as 512.
RUN_PEAK_MB = 512
VALGRIND_PEAK_MB = 3400
STARTUP_S = 120   # (a run under valgrind reaches its peak in its first minute: one started since counts its whole peak)


def capped_room_mb():
    """The room left under the cgroups above this process with a memory.max (Linux, cgroup v2), in MB: the least of max
    minus current over them (a job's scope and rcheck.slice both have 15 GB; the slice is shared). None where there is
    no such cap (the Mac, or uncapped cgroups)."""
    try:
        line = next(l for l in open('/proc/self/cgroup') if l.startswith('0::'))
    except (OSError, StopIteration):
        return None
    path, room = '/sys/fs/cgroup' + line[3:].strip(), None
    while path.startswith('/sys/fs/cgroup/'):
        try:
            cap = open(os.path.join(path, 'memory.max')).read().strip()
            if cap != 'max':
                left = (int(cap) - int(open(os.path.join(path, 'memory.current')).read())) >> 20
                room = left if room is None else min(room, left)
        except (OSError, ValueError):
            pass
        path = os.path.dirname(path)
    return room


def run_peak_mb(cmd):
    """The expected peak of a run: under valgrind (career_ab.py run --taint or --isolate) or native."""
    return VALGRIND_PEAK_MB if ('--taint' in cmd or '--isolate' in cmd) else RUN_PEAK_MB


def run_times(path):
    """CAREER_AB_TIMES: {'<folder>/<run>': seconds} of the last runs that ended (run_jobs keeps it), or {}."""
    try:
        return json.load(open(path))
    except (OSError, ValueError):
        return {}


def keep_times(path, ended):
    """The runs' seconds into CAREER_AB_TIMES (several gates' parts write it: under its lock, merged)."""
    with open(path + '.lock', 'a+') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        times = run_times(path)
        times.update(ended)
        with open(path + '.tmp', 'w') as f:
            json.dump(times, f, indent=0, sort_keys=True)
        os.replace(path + '.tmp', path)


def run_jobs(jobs, root, limit):
    """(name, command) one after another, at most `limit` at a time, while memory allows: 30% free on the host, and,
    under a cgroup's memory cap, room for the next run's peak beside the peaks still ahead of the runs just started.
    Two things only the Mac gate (remote/mac/gate.sh) sets, which change no run, only when each starts: RUNNER_POOL,
    the job's runners shared by all its parts (runner_pool.py: a run starts when it has one of them, and gives it back
    when it ends), and CAREER_AB_TIMES, the seconds of each run of the gates before (a folder's base name and the run's,
    ab/editor-on), with which the longest runs start first (a run it has no time for first of all, in its order) and
    the folder's parts end close together (int27gate's AI A/B: its last run started 2 minutes before the others' end)."""
    import runner_pool
    pool = runner_pool.Pool.from_env()
    times_path = os.environ.get('CAREER_AB_TIMES')
    jobs = list(jobs)
    if times_path:
        times, base = run_times(times_path), os.path.basename(os.path.normpath(root))
        known = lambda job: times.get('%s/%s' % (base, job[0]))
        jobs = [j for j in jobs if known(j) is None] + sorted((j for j in jobs if known(j) is not None), key=known, reverse=True)
    running = []
    said = False
    while jobs or running:
        ended = {}
        for job in list(running):
            if job[1].poll() is not None:
                running.remove(job)
                if job[4] is not None:
                    job[4].close()   # (the pool's runner back)
                ended['%s/%s' % (os.path.basename(os.path.normpath(root)), job[0])] = round(time.time() - job[2])
                print('%s %s exit %d' % (time.strftime('%H:%M:%S'), job[0], job[1].returncode), flush=True)
        if ended and times_path:
            keep_times(times_path, ended)
        if pool is not None:
            pool.note('want', len(jobs))
        if jobs and len(running) < limit and free_memory() >= 30:
            room = capped_room_mb()
            need = run_peak_mb(jobs[0][1]) + sum(r[3] for r in running if time.time() - r[2] < STARTUP_S)
            if room is not None and room < need:
                if not said:
                    print('%s %s waits: %d MB under the cgroup\'s cap, %d MB wanted (its peak and the peaks ahead of '
                          'the runs just started)' % (time.strftime('%H:%M:%S'), jobs[0][0], room, need), flush=True)
                    said = True
            else:
                runner = pool.take() if pool is not None else None
                if pool is None or runner is not None:
                    name, cmd = jobs.pop(0)
                    os.makedirs(root, exist_ok=True)
                    log = open(os.path.join(root, name + '.gate.log'), 'w')
                    running.append((name, subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT), time.time(),
                                    run_peak_mb(cmd), runner))
                    said = False
        time.sleep(1)
    if pool is not None:
        pool.note('want', 0)


def voice_calls(directory):
    """{callee: [(tick, arguments it takes)]} of a run's voice.txt, in order. A call made as C++ logs no registers
    (`ADDRESS COUNT TICK CYCLE direct sp=.. ctxsp=..`, seven words): its arguments are None."""
    calls = collections.defaultdict(list)
    table = voice_table(run_game(directory))
    for line in open(os.path.join(directory, 'voice.txt'), errors='replace'):
        word = line.split()
        if len(word) < 7 or 'ret' in word[1:3] or (len(word) < 8 and word[4] != 'direct'):
            continue
        try:
            address = int(word[0], 16)
        except ValueError:
            continue
        if address in table:
            direct = word[4] == 'direct'
            calls[address].append((int(word[2]), None if direct else tuple(word[4 + i] for i in table[address][1])))
    return calls


def run_game(directory):
    """The game a run was made for (its run.txt's `game` line; AI's when it has none)."""
    for line in open(os.path.join(directory, 'run.txt')):
        if line.startswith('game '):
            return line.split()[1]
    return 'ai'


def compare_voices(a_dir, b_dir, report, problems, listed):
    a, b = voice_calls(a_dir), voice_calls(b_dir)
    # (a run stops a few ticks past its limit: what comes after the last tick both reached is not compared)
    last = min(max((c[-1][0] for c in calls.values() if c), default=0) for calls in (a, b))
    report('voice calls to tick %d' % last)
    for address, (name, _) in sorted(voice_table(run_game(a_dir)).items()):
        x = [c for c in a.get(address, []) if c[0] < last]
        y = [c for c in b.get(address, []) if c[0] < last]
        # (a C++ call has no arguments to compare, only its tick)
        differ = [i for i in range(min(len(x), len(y)))
                  if x[i][0] != y[i][0] or (x[i][1] is not None and y[i][1] is not None and x[i][1] != y[i][1])]
        report('voice %-14s %s and %s calls, %s differ%s' % (name, format(len(x), ','), format(len(y), ','), format(len(differ), ','),
                                                             ', first %s and %s' % (x[differ[0]], y[differ[0]]) if differ else ''))
        if differ or len(x) != len(y):
            problems.append('%s: %d of %d calls differ, %d and %d made' % (name, len(differ), min(len(x), len(y)), len(x), len(y)))


# The inputs that use the app's Settings page in the game's menus (a route's ZX_GAME_SETTINGS_MENUS=1 and
# ZX_TRACE_SETTINGS=1): a run of one that never reaches the page fails, as one whose page reads back wrong.
PAGE_INPUTS = ('menus',)
PAGE_KEY = re.compile(r"^\[settings\] page (0x[0-9a-f]+) (\w+): (\S+) (\S*) '(.*)' -> '(.*)'$")


def page_keys(directory):
    """The Settings page's keys in a run's log (zge_settings.cpp's ZX_TRACE_SETTINGS lines): [(dialog, key, section,
    row, value before, value the page shows after)]."""
    logs = [p for p in glob.glob(os.path.join(directory, '*.log')) if os.path.basename(p) != 'state.log']
    out = []
    for path in logs[:1]:
        for line in open(path, errors='replace'):
            m = PAGE_KEY.match(line.rstrip('\n'))
            if m:
                out.append(m.groups())
    return out


def page_readback(keys):
    """(Right/Left pairs, those at an end, those wrong): each Right the route makes is followed by a Left on the same
    row, which finds the value the Right showed (nothing set it back in between) and, when the Right changed it, shows
    the value before the Right again. A Right that leaves the value as it was is a row at its end (a range at its top,
    an action), whose Left is then the row's first step."""
    pairs = ends = wrong = 0
    for i, (dialog, key, section, row, before, after) in enumerate(keys):
        if key != 'Right':
            continue
        nxt = keys[i + 1] if i + 1 < len(keys) else None
        pairs += 1
        if not nxt or nxt[1] != 'Left' or nxt[3] != row or nxt[4] != after:
            wrong += 1
        elif after == before:
            ends += 1
        elif nxt[5] != before:
            wrong += 1
    return pairs, ends, wrong


def compare_page(a_dir, b_dir, report, problems):
    """The Settings page's keys of two runs: each side's read back (page_readback) and the two the same, key by key."""
    a, b = page_keys(a_dir), page_keys(b_dir)
    info = os.path.join(a_dir, 'run.txt')
    name = re.search(r'^input (\S+)$', open(info).read(), re.M) if os.path.exists(info) else None
    if not a and not b:
        if name and name.group(1) in PAGE_INPUTS:
            report('Settings page: never reached (no ZX_TRACE_SETTINGS page keys in either log)')
            problems.append('the Settings page never reached')
        return
    for side, keys in (('A', a), ('B', b)):
        pairs, ends, wrong = page_readback(keys)
        report('Settings page %s: %d keys on %d visits, %d Right/Left pairs read back%s (%d at an end)' % (
            side, len(keys), len({k[0] for k in keys if k[1] == 'Open'}), pairs - wrong,
            ', %d wrong' % wrong if wrong else '', ends))
        if wrong or not pairs:
            problems.append('Settings page %s: %d of %d Right/Left pairs read back wrong' % (side, wrong, pairs))
    differ = [i for i in range(min(len(a), len(b))) if a[i] != b[i]]
    if differ or len(a) != len(b):
        first = differ[0] if differ else min(len(a), len(b))
        report('Settings page: the keys differ from key %d: %s and %s' % (first, a[first:first + 1], b[first:first + 1]))
        problems.append('Settings page keys: %d of %d differ, %d and %d' % (len(differ), min(len(a), len(b)), len(a), len(b)))


def stale(args):
    """Every input, `off` against the defaults, both under each fill of --fills: see the module's text."""
    tool = os.path.abspath(__file__)
    root = os.path.abspath(args.dir)
    os.makedirs(root, exist_ok=True)
    held = hold(os.path.join(root, '.gate.lock'), 'the folder %s' % root)  # noqa: F841 (held to exit)
    other = GAMES.get(args.game)
    gate_inputs = other['gate'] if other else GATE_INPUTS
    allow = args.allow or ','.join(os.path.join(os.path.dirname(tool), f) for f in (other['allow'] if other else 'ab_allow.txt').split(','))
    inputs = dict(gate_inputs)
    names = args.inputs.split(',') if args.inputs else [n for n, _ in gate_inputs]
    fills = [(STALE_FILLS[f], f) for f in args.fills.split(',')]
    jobs = []
    for name in names:
        for (tag, flag, _), _fill in fills:
            for side, spec in (('off', ['--spec', args.off_spec]), ('on', ['--spec', args.spec] if args.spec else [])):
                label = '%s-%s-%s' % (name, tag, side)
                jobs.append((label, [sys.executable, '-I', tool, 'run', label, '--dir', root, '--input', name, '--to', str(inputs[name]),
                                     '--runner', args.runner, '--clock', str(args.clock), '--secs', str(args.secs), '--voices', flag,
                                     '--game', args.game] + spec
                                    + (['--stores', os.path.abspath(args.stores)] if args.stores else [])))
    run_jobs(jobs, root, args.jobs)
    failed = []
    for name in names:
        bad = False
        for (tag, _, report), fill in fills:
            a, b = os.path.join(root, '%s-%s-off' % (name, tag)), os.path.join(root, '%s-%s-on' % (name, tag))
            cmd = [sys.executable, '-I', tool, 'compare', a, b, '--allow', allow, '--out', os.path.join(root, '%s-%s.words.txt' % (name, tag))]
            r = subprocess.run(cmd, capture_output=True, text=True)
            lines, problems = [], []
            compare_voices(a, b, lines.append, problems, args.list)
            with open(os.path.join(root, name + report), 'w') as f:
                f.write(r.stdout + r.stderr + '\n'.join(lines) + '\n')
            line = re.search(r'^RESULT: (.*)$', r.stdout, re.M)
            ok = r.returncode == 0 and not problems
            print('%-15s %-6s %s: %s%s' % (name, fill, 'PASS' if ok else 'FAIL', line.group(1) if line else 'no result',
                                           '; ' + '; '.join(problems[:4]) if problems else ''), flush=True)
            if not ok:
                bad = True
            elif not args.keep:
                for path in glob.glob(os.path.join(a, 'dump-*.ram')) + glob.glob(os.path.join(b, 'dump-*.ram')):
                    os.remove(path)
        if bad:
            failed.append(name)
    print('stale at clock %d (%s): %d of %d inputs pass%s' % (args.clock, args.fills, len(names) - len(failed), len(names),
                                                              ': failed ' + ','.join(failed) if failed else ''), flush=True)
    return 1 if failed else 0


def gate(args):
    """Every input, `off` against the defaults (or --spec) at one clock: see the module's text."""
    tool = os.path.abspath(__file__)
    root = os.path.abspath(args.dir)
    os.makedirs(root, exist_ok=True)
    held = hold(os.path.join(root, '.gate.lock'), 'the folder %s' % root)  # noqa: F841 (held to exit)
    other = GAMES.get(args.game)
    gate_inputs = other['gate'] if other else GATE_INPUTS
    allow = args.allow or ','.join(os.path.join(os.path.dirname(tool), f) for f in (other['allow'] if other else 'ab_allow.txt').split(','))
    inputs = dict(gate_inputs)
    names = args.inputs.split(',') if args.inputs else [n for n, _ in gate_inputs]
    for name in names:
        if other and name not in inputs:  # a played route of tools/dm2routes.py: to its end
            script = subprocess.check_output([sys.executable, os.path.join(dm2_repo(), 'tools', 'dm2routes.py'), 'script', name], text=True)
            meta = os.path.join(SCRATCH_ROOT, 'playdm2', 'sessions', name[len('play-'):], 'meta.json')
            inputs[name] = json.load(open(meta))['end_tick'] if os.path.exists(meta) else max(int(i.split('v')[0]) for i in script.split(',')) + 20000
    jobs = []
    for name in names:
        for side, spec in (('off', ['--spec', args.off_spec]), ('on', ['--spec', args.spec] if args.spec else [])):
            jobs.append(('%s-%s' % (name, side), [sys.executable, '-I', tool, 'run', '%s-%s' % (name, side), '--dir', root, '--input', name,
                                                  '--to', str(inputs[name]), '--runner', args.runner, '--clock', str(args.clock),
                                                  '--secs', str(args.secs), '--game', args.game] + spec + (['--voices'] if args.voices else [])
                                                 + (['--every', str(args.every)] if args.every else [])))
    run_jobs(jobs, root, args.jobs)
    failed = []
    for name in names:
        a, b = os.path.join(root, name + '-off'), os.path.join(root, name + '-on')
        cmd = [sys.executable, '-I', tool, 'compare', a, b, '--allow', allow, '--out', os.path.join(root, name + '.words.txt')]
        if other and name in other.get('ram_information', ()):
            cmd.append('--ram-information')
        r = subprocess.run(cmd, capture_output=True, text=True)
        with open(os.path.join(root, name + '.compare.txt'), 'w') as f:
            f.write(r.stdout + r.stderr)
        line = re.search(r'^RESULT: (.*)$', r.stdout, re.M)
        fields = re.search(r'(\d+ of \d+) fields equal', r.stdout)
        tick = re.search(r'compared at every tick to tick (\d+)', r.stdout)
        page = re.search(r'^Settings page B: (.*)$', r.stdout, re.M)  # (a run that uses the page: its keys read back)
        print('%-9s %s: %s fields to tick %s; %s%s' % (name, 'PASS' if r.returncode == 0 else 'FAIL', fields.group(1) if fields else '?',
                                                       tick.group(1) if tick else '?', line.group(1) if line else 'no result',
                                                       '; page ' + page.group(1) if page else ''), flush=True)
        # Both sides stopping at the same tick (a hang, a crash in the code both run) compare the same: a run that ends
        # short of its input's end fails as a difference does. How far a side got is the tick run-game.sh stopped it at
        # (its gate log), not the compare's last tick: that is the last change of the state's fields, which a menu or
        # the editor leaves long before the input's end.
        reached = []
        for side in ('off', 'on'):
            try:
                stops = re.findall(r'^stopped at tick (\d+) ', open(os.path.join(root, '%s-%s.gate.log' % (name, side))).read(), re.M)
            except OSError:
                stops = []
            reached.append(int(stops[-1]) if stops else 0)
        short = min(reached) + 60 < inputs[name]
        if short:
            print('%-9s FAIL: off reached tick %d, on %d, the input goes to %d (a side stopped short: a hang, a crash or '
                  'the time limit)' % (name, reached[0], reached[1], inputs[name]), flush=True)
        # An input's own lines each side's log must have, and must not (GAMES' log_lines: pe-session's park loaded)
        wrong_log = []
        for side in ('off', 'on'):
            try:
                text = open(os.path.join(root, '%s-%s' % (name, side), '%s-%s.log' % (name, side)), errors='replace').read()
            except OSError:
                text = ''
            for want, must in (other.get('log_lines', {}).get(name, ()) if other else ()):
                if (want in text) != must:
                    wrong_log.append('%s %s %r' % (side, 'lacks' if must else 'has', want))
        if wrong_log:
            print('%-9s FAIL: the run log: %s' % (name, '; '.join(wrong_log)), flush=True)
        if r.returncode or short or wrong_log:
            failed.append(name)
        elif not args.keep:
            for path in glob.glob(os.path.join(a, 'dump-*.ram')) + glob.glob(os.path.join(b, 'dump-*.ram')):
                os.remove(path)
    print('gate at clock %d: %d of %d inputs pass%s' % (args.clock, len(names) - len(failed), len(names), ': failed ' + ','.join(failed) if failed else ''))
    return 1 if failed else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    r = sub.add_parser('run')
    r.add_argument('name')
    r.add_argument('--dir', required=True)
    r.add_argument('--spec')
    r.add_argument('--to', type=int, default=20000)
    r.add_argument('--runner', default=os.environ.get('RUNNER'), required='RUNNER' not in os.environ)
    r.add_argument('--secs', type=int, default=900)
    r.add_argument('--every', type=int, default=3000)
    r.add_argument('--fields')
    r.add_argument('--game', default='ai', choices=('ai',) + tuple(GAMES))
    r.add_argument('--input', default='career')
    r.add_argument('--packet-dump')
    r.add_argument('--voices', action='store_true', help='log every voice call\'s arguments (voice.txt)')
    r.add_argument('--poison', action='store_true', help='ZX_STACK_POISON=1')
    r.add_argument('--poison-stores', action='store_true', help='ZX_STACK_POISON_STORES=readable_frame_stores.tsv')
    r.add_argument('--poison-frames', action='store_true', help='ZX_STACK_POISON_FRAMES=readable_frame_stores.tsv')
    r.add_argument('--stores', help='with --poison-stores, --poison-frames: another table (a part of it, to find which function a difference is)')
    r.add_argument('--clock', type=int, default=1, help='ZX_EE_SCALE (1: the PS2\'s clock)')
    r.set_defaults(func=run)
    g = sub.add_parser('gate')
    g.add_argument('--dir', required=True)
    g.add_argument('--runner', default=os.environ.get('RUNNER'), required='RUNNER' not in os.environ)
    g.add_argument('--clock', type=int, default=GATE_CLOCK)
    g.add_argument('--inputs')
    g.add_argument('--jobs', type=int, default=2)
    g.add_argument('--secs', type=int, default=1200)
    g.add_argument('--allow')
    g.add_argument('--game', default='ai', choices=('ai',) + tuple(GAMES))
    g.add_argument('--keep', action='store_true')
    g.add_argument('--voices', action='store_true', help='log the voice calls too (voice.txt)')
    g.add_argument('--every', type=int, help='a RAM dump every N ticks (run\'s default: 3,000); 500 sees what a dump every '
                   '3,000 ticks misses (bmxw1: a QueryHit\'s w that lives a frame)')
    g.add_argument('--spec', help='the on side\'s ZX_READABLE (tested: the gate of a game\'s Tested functions); default the defaults')
    g.add_argument('--off-spec', default='off', help='the off side\'s ZX_READABLE (default off). A setting that needs a '
                   'readable body on both sides: off with that one on, e.g. off,+newRider for DM2\'s Create Player, which '
                   'turns itself down where readable newRider does not run (tools29)')
    t = sub.add_parser('stale')
    t.add_argument('--dir', required=True)
    t.add_argument('--runner', default=os.environ.get('RUNNER'), required='RUNNER' not in os.environ)
    t.add_argument('--clock', type=int, default=GATE_CLOCK)
    t.add_argument('--inputs')
    t.add_argument('--jobs', type=int, default=2)
    t.add_argument('--secs', type=int, default=1200)
    t.add_argument('--list', type=int, default=8)
    t.add_argument('--allow')
    t.add_argument('--keep', action='store_true')
    t.add_argument('--fills', default='stores,frames', help='stores, frames or both (the module\'s text)')
    t.add_argument('--stores', help='another table than readable_frame_stores.tsv (as `run`\'s)')
    t.add_argument('--game', default='ai', choices=('ai',) + tuple(GAMES))
    t.add_argument('--spec', help='the on side\'s ZX_READABLE (as gate\'s); default the defaults')
    t.add_argument('--off-spec', default='off', help='the off side\'s ZX_READABLE (default off). A setting that needs a '
                   'readable body on both sides: off with that one on, e.g. off,+newRider for DM2\'s Create Player, which '
                   'turns itself down where readable newRider does not run (tools29)')
    t.set_defaults(func=stale)
    g.set_defaults(func=gate)
    c = sub.add_parser('compare')
    c.add_argument('a')
    c.add_argument('b')
    c.add_argument('--out')
    c.add_argument('--list', type=int, default=8)
    c.add_argument('--allow')
    c.add_argument('--dumps-only', action='store_true')
    c.add_argument('--state-only', action='store_true')
    c.add_argument('--ram-information', action='store_true', help='RAM differences reported, not counted (gate: GAMES\' ram_information)')
    c.set_defaults(func=compare)
    s = sub.add_parser('script')
    s.add_argument('trace')
    s.add_argument('out')
    s.add_argument('env', nargs='*')
    s.add_argument('--pokes', help='a ZX_POKE_TRACE file')
    s.set_defaults(func=script_of)
    p = sub.add_parser('packets')
    p.add_argument('a')
    p.add_argument('b')
    p.add_argument('tick', type=int)
    p.add_argument('stream', choices=PACKET_STREAMS)
    p.add_argument('--list', type=int, default=8)
    p.set_defaults(func=packets)
    args = parser.parse_args()
    refuse_main(args)
    sys.exit(args.func(args))


def refuse_main(args):
    """Stopped before anything is written when the working directory or --dir, --out (a script's out) is inside the
    main checkout, unless the orchestrator's merge and batch scripts run it (ZAXIS_IN_MAIN=1): the tooling's
    zguard.py (tools36). A worker's copy of the checks has no tooling around it, and no main checkout to guard."""
    path = os.path.join(ztree.TOOLING, 'zguard.py')
    if not os.path.isfile(path):
        return
    import importlib.util
    spec = importlib.util.spec_from_file_location('zguard', path)
    zguard = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(zguard)
    zguard.refuse('career_ab.py', [getattr(args, 'dir', None), getattr(args, 'out', None)])


if __name__ == '__main__':
    main()
