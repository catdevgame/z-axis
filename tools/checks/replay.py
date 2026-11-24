#!/usr/bin/env python3
"""replay.py: the player's own play sessions (runtime/play_recording.h), replayed here.

A recording (.zrec) is one boot of one game made by the app's Settings > Game > Record Play Sessions
(or ZX_RECORD=<folder> on any run): what the game's thread read from the host (the pads as the
game saw them, mouse-look, shortcuts, dates, the picture's aspect), the boot's settings, the saves at
boot, state checksums every 60 vsyncs and each host frame's telemetry, all on the guest's vsync clock.
Nothing is asked of the player while playing: the moments worth a look are found here, from the
telemetry.

    replay.py info FILE                 the header, the size, what is in it
    replay.py moments FILE [--json]     hitches, slow stretches, pacer drops, setting changes, the end
                                        (a file without its end record: a crash or a kill), restarts
    replay.py telemetry FILE [--csv OUT] [--from V] [--to V]
                                        per host frame: vsync, frame ms, game/VIF1/GS/host CPU ms, flips
    replay.py run FILE [--to V | --moment N [--before V]] [--from V] [--runner R] [--name NAME]
                  [--record] [--picture] [--as-is] [--readable SPEC] [--env K=V ...] [--max-secs S]
                                        replays it unpaced, host input off, null GS, from power-on with the
                                        recorded saves and settings, to vsync V (a chunk: boot to a moment)
                                        or the end; prints the runtime's checksum verdict. A recording the
                                        app made (header front_end=app) replays through the app's front end
                                        (RUNNER --game GAME, this Mac's disc folder of the game), as it boots
                                        differently from a runner given the ELF. --from V: the
                                        traces and profiles in --env start there (ZX_*_FROM=V), a window.
                                        --record: the replay records too (to compare with `compare`).
                                        An AI recording from before b128 (no settings room in zgui's
                                        heap) is compared with that heap's two links and the room as its
                                        build had them, every other word as it is (SettingsRoomView;
                                        --as-is: the runtime's own comparison alone).
                                        The readable functions on are the ones the recording's build ran
                                        (its commit's Verified rows and its own ZX_READABLE, by address):
                                        the readable code does not charge the original's EE cycles, so a
                                        function promoted since moves the vsync a read falls in. --readable
                                        SPEC: the runner's set with SPEC applied; --readable runner: as it is.
    replay.py twice FILE --at V[,V...] [--runner R]
                                        replays it twice with RAM dumps at those vsyncs and compares them
                                        byte for byte (and both against the recording's checksums)
    replay.py compare A.zrec B.zrec     the two recordings' checksums at every vsync both have
    replay.py route FILE OUT_DIR [--runner R] [--to V]
                                        a corpus recording run over the session (ZX_FNREC=all into
                                        OUT_DIR, readable off), for zx-fncheck and the coverage tools
    replay.py extend FILE OUT --to V [PRESS ...]
                                        a recording cut short (a crash, a hang the app was killed in) given what
                                        it lost: port 0's presses after its last input (NAME@VSYNC:HOLD, e.g.
                                        DOWN@59765:4 CROSS@59810:4; one pad read a vsync, as at its end) and a
                                        last vsync V, so `run --to V` replays past its end, its inputs held
                                        (the runtime replays a cut-short file past its end; an ended one stops)
    replay.py script FILE OUT           port 0 as a ZX_PAD_SCRIPT (vsync clock), for tools that take a
                                        pad script (full pressure; ZX_REPLAY is the exact route)
    replay.py pull [--serial S] [--to DIR]
                                        the Thor's recordings (adb, our package's external files folder)
    replay.py list [DIR]                the recordings in a folder (default: the Mac app's)

Runs go in $ZAXIS_SCRATCH/replays/<name> (RUN_DIR overrides) with a scratch HOME, card, support
folder and cwd; each has a time limit (--max-secs, default 1800) and stops itself at its vsync.
"""

import argparse
import glob
import json
import os
import shutil
import statistics
import struct
import subprocess
import sys
import time

TOOLS = os.path.dirname(os.path.abspath(__file__))
TOOLING = os.path.dirname(TOOLS)     # (tools/: zpaths.py, zguard.py)
sys.path.insert(0, TOOLING)
import zpaths  # noqa: E402
import zguard  # noqa: E402

MAGIC = b'ZAXREC1\n'
HEADER, FILE, INPUT, CHECKSUM, FRAME, EVENT, END = 1, 2, 3, 4, 5, 6, 7
CHANNELS = ['pad0', 'pad1', 'port-connected', 'hotkey', 'mouse-look', 'key-recent', 'prompt-device',
            'prompt-generation', 'free-input', 'date', 'view-aspect', 'widescreen', 'random', 'draw-distance',
            'motion-blur', 'native-draws', 'setting', 'top-screen-hud']
HOTKEY_RESTART = 0  # ps2_host_input::kHotkeyRestart
GAME_ELF = {'ai': 'SLUS_203.27', 'dm2': 'SLUS_201.59', 'bmx': 'SLUS_204.15'}
PACKAGE = 'rest.src.cat.aggressiveinline'
THOR = zpaths.PATHS.get('TEST_DEVICE_SERIAL')   # the Android test device's adb serial (paths.env's names; none: adb's own choice)
PAD_BUTTONS = {0x0080: 'LEFT', 0x0040: 'DOWN', 0x0020: 'RIGHT', 0x0010: 'UP', 0x0008: 'START', 0x0004: 'R3',
               0x0002: 'L3', 0x0001: 'SELECT', 0x8000: 'SQUARE', 0x4000: 'CROSS', 0x2000: 'CIRCLE',
               0x1000: 'TRIANGLE', 0x0800: 'R1', 0x0400: 'L1', 0x0200: 'R2', 0x0100: 'L2'}


# ---- reading a recording ------------------------------------------------------------------------------------

class Reader:
    def __init__(self, data, pos=0, end=None):
        self.data, self.pos, self.end = data, pos, len(data) if end is None else end

    def varint(self):
        value, shift = 0, 0
        while True:
            if self.pos >= self.end:
                raise EOFError
            b = self.data[self.pos]
            self.pos += 1
            value |= (b & 0x7F) << shift
            if not b & 0x80:
                return value
            shift += 7

    def string(self):
        n = self.varint()
        if self.pos + n > self.end:
            raise EOFError
        s = self.data[self.pos:self.pos + n]
        self.pos += n
        return s


class Recording:
    """A .zrec read whole: header, files, input changes, checksums, frames, events, end."""

    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            data = f.read()
        if not data.startswith(MAGIC):
            sys.exit('%s: not a play recording' % path)
        self.size = len(data)
        self.header = {}
        self.header_lines = []
        self.files = {}
        self.inputs = []      # (channel, key, read, vsync, value bytes)
        self.checksums = {}   # vsync -> [u32]
        self.frames = []      # (vsync, frame_ms, game, vif1, gs, host, flips, dropped_ms)
        self.events = []      # (vsync, kind, text)
        self.end = None       # (vsync, why) or None: cut short
        self.bytes_by_type = {}
        self.cut_short_bytes = 0
        last = {}
        r = Reader(data, len(MAGIC))
        while r.pos < len(data):
            start = r.pos
            try:
                kind = data[r.pos]
                r.pos += 1
                n = r.varint()
                if r.pos + n > len(data):
                    raise EOFError
            except EOFError:
                self.cut_short_bytes = len(data) - start
                break
            p = Reader(data, r.pos, r.pos + n)
            r.pos += n
            self.bytes_by_type[kind] = self.bytes_by_type.get(kind, 0) + (r.pos - start)
            try:
                self._record(kind, p, last)
            except EOFError:
                self.cut_short_bytes = len(data) - start
                break

    def _record(self, kind, p, last):
        if kind == HEADER:
            text = p.data[p.pos:p.end].decode('utf-8', 'replace')
            for line in text.splitlines():
                self.header_lines.append(line)
                k, _, v = line.partition('=')
                if k == 'file':
                    continue
                self.header.setdefault(k, v)
        elif kind == FILE:
            name = p.string().decode('utf-8', 'replace')
            self.files[name] = bytes(p.string())
        elif kind == INPUT:
            channel, key, since, vsync, size, mask = (p.varint() for _ in range(6))
            prev = last.get((channel, key))
            value = bytearray(prev[1] if prev else bytes(size))
            value = (value + bytes(size))[:size]
            whole = mask == 0 and size > 63
            for i in range(size):
                if whole or (i < 64 and (mask >> i) & 1):
                    if p.pos >= p.end:
                        raise EOFError
                    value[i] = p.data[p.pos]
                    p.pos += 1
            read = since if prev is None else prev[0] + since
            last[(channel, key)] = (read, bytes(value))
            self.inputs.append((channel, key, read, vsync, bytes(value)))
        elif kind == CHECKSUM:
            vsync, count = p.varint(), p.varint()
            if p.pos + 4 * count > p.end:
                raise EOFError
            self.checksums[vsync] = list(struct.unpack_from('<%dI' % count, p.data, p.pos))
        elif kind == FRAME:
            v = [p.varint() for _ in range(8)]
            self.frames.append((v[0], v[1] / 1000.0, v[2] / 1000.0, v[3] / 1000.0, v[4] / 1000.0, v[5] / 1000.0,
                                v[6], v[7] / 1000.0))
        elif kind == EVENT:
            vsync = p.varint()
            self.events.append((vsync, p.string().decode(), p.string().decode('utf-8', 'replace')))
        elif kind == END:
            vsync = p.varint()
            self.end = (vsync, p.string().decode('utf-8', 'replace'))

    @property
    def game(self):
        game = self.header.get('game', '')
        if not game:  # (a runner given an ELF before the game was named by it)
            game = {elf: gid for gid, elf in GAME_ELF.items()}.get(self.header.get('elf', '').upper(), '')
        return game

    @property
    def from_app(self):
        """Made through the app's front end (its menu, folders, settings): replayed through it too."""
        return self.header.get('front_end') == 'app'

    def last_vsync(self):
        if self.end:
            return self.end[0]
        candidates = [v for v in self.checksums] + [f[0] for f in self.frames] + [i[3] for i in self.inputs]
        return max(candidates) if candidates else 0

    def boot_values(self):
        return {k[5:]: v for k, v in self.header.items() if k.startswith('boot.')}

    def env_values(self):
        return {k[4:]: v for k, v in self.header.items() if k.startswith('env.')}


# ---- moments: what the telemetry shows ------------------------------------------------------------------------

def moments(rec):
    """Detected moments, by vsync: each a dict (vsync, kind, detail)."""
    found = []
    frames = rec.frames
    if frames:
        times = [f[1] for f in frames]
        median = statistics.median(times)
        hitch = max(50.0, 3.0 * median)
        for f in frames:
            if f[1] >= hitch:
                found.append({'vsync': f[0], 'kind': 'hitch', 'detail': '%.1f ms frame (median %.1f)' % (f[1], median)})
        # The pacer dropping catch-up after a stall: one moment per burst (frames within a second of each other).
        burst = None
        for f in frames + [(None,) * 8]:
            ended = f[0] is None or (burst is not None and f[0] - burst[1] > 60)
            if burst is not None and ended:
                found.append({'vsync': burst[0], 'kind': 'pacer-drop', 'detail': 'pacer dropped %.1f ms to vsync %d'
                              % (burst[2], burst[1]), 'until': burst[1]})
                burst = None
            if f[0] is not None and f[7] > 0.0:
                burst = [f[0], f[0], f[7]] if burst is None else [burst[0], f[0], burst[2] + f[7]]
        # Slow: a second of host time in which the game made under 54 vsyncs (90 % of 59.94) but did advance
        # (no vsync at all is a stall: a load, or the game paused), consecutive seconds merged.
        seconds, start, wall = [], 0, 0.0
        for k, f in enumerate(frames):
            wall += f[1]
            if wall >= 1000.0:
                seconds.append((frames[start][0], f[0], f[0] - frames[start][0], wall))
                start, wall = k + 1, 0.0
        run = None
        for first, last, vsyncs, ms in seconds + [(None, None, 60, 1000.0)]:
            rate = vsyncs * 1000.0 / ms
            if first is not None and 0 < rate < 54.0:
                run = [first, last, rate] if run is None else [run[0], last, min(run[2], rate)]
            elif run is not None:
                found.append({'vsync': run[0], 'kind': 'slow', 'detail': 'to vsync %d: the game at %.0f vsyncs/s at '
                              'worst' % (run[1], run[2]), 'until': run[1]})
                run = None
        # The game itself stalling: host frames go by, vsyncs do not (a load, or the game paused).
        stall = None
        for a, b in zip(frames, frames[1:] + [(None,) * 8]):
            if b[0] is not None and b[0] == a[0]:
                stall = [a[0], (stall[1] if stall else 0.0) + b[1]]
                continue
            if stall is not None and stall[1] >= 250.0:
                found.append({'vsync': stall[0], 'kind': 'stall', 'detail': 'no vsync for %.0f ms' % stall[1]})
            stall = None
    for vsync, kind, text in rec.events:
        if kind == 'setting':
            found.append({'vsync': vsync, 'kind': 'setting', 'detail': text + ' (from the app menu: the replay applies '
                          'it at this vsync)'})
        elif kind == 'error':
            found.append({'vsync': vsync, 'kind': 'error', 'detail': text})
    prev = None
    for channel, key, read, vsync, value in rec.inputs:
        if CHANNELS[channel] == 'hotkey' and key == HOTKEY_RESTART:
            count = struct.unpack('<I', value[:4])[0]
            if prev is not None and count != prev:
                found.append({'vsync': vsync, 'kind': 'restart', 'detail': 'the restart shortcut'})
            prev = count
    if rec.end is None:
        found.append({'vsync': rec.last_vsync(), 'kind': 'crash', 'detail': 'the file has no end record: the app '
                      'crashed or was killed after vsync %d' % rec.last_vsync()})
    else:
        found.append({'vsync': rec.end[0], 'kind': 'end', 'detail': rec.end[1]})
    found.sort(key=lambda m: (m['vsync'], m['kind']))
    return found


# ---- commands -------------------------------------------------------------------------------------------------

def cmd_info(args):
    rec = Recording(args.file)
    names = {HEADER: 'header', FILE: 'saves', INPUT: 'inputs', CHECKSUM: 'checksums', FRAME: 'telemetry',
             EVENT: 'events', END: 'end'}
    last = rec.last_vsync()
    print('%s: %s, %d bytes, vsync 0 to %d (%.1f min of game time), %s' % (
        args.file, rec.game, rec.size, last, last / 59.94 / 60.0,
        'ended: %s' % rec.end[1] if rec.end else 'no end record (crash or kill)'))
    for k in ('commit', 'started', 'front_end', 'device', 'cpu', 'os', 'power', 'thermal', 'battery', 'cores',
              'core_tiers', 'elf', 'elf_hash', 'boot'):
        if k in rec.header:
            print('  %-9s %s' % (k, rec.header[k]))
    print('  size by part: ' + ', '.join('%s %d' % (names.get(k, k), v) for k, v in sorted(rec.bytes_by_type.items())))
    by_channel = {}
    for channel, key, *_ in rec.inputs:
        by_channel[CHANNELS[channel]] = by_channel.get(CHANNELS[channel], 0) + 1
    print('  input changes: ' + ', '.join('%s %d' % kv for kv in sorted(by_channel.items())))
    print('  saves at boot: %d files, %d bytes' % (len(rec.files), sum(len(v) for v in rec.files.values())))
    print('  checksums: %d, frames: %d, events: %d' % (len(rec.checksums), len(rec.frames), len(rec.events)))
    for k, v in sorted(rec.boot_values().items()):
        print('  boot %s=%s' % (k, v))
    for k, v in sorted(rec.env_values().items()):
        print('  env %s=%s' % (k, v))


def cmd_moments(args):
    rec = Recording(args.file)
    found = moments(rec)
    if args.json:
        print(json.dumps(found, indent=1))
        return
    for i, m in enumerate(found):
        print('%3d  vsync %7d  %-10s %s' % (i, m['vsync'], m['kind'], m['detail']))


def cmd_telemetry(args):
    rec = Recording(args.file)
    out = open(args.csv, 'w') if args.csv else sys.stdout
    out.write('vsync,frame_ms,game_cpu_ms,vif1_cpu_ms,gs_cpu_ms,host_cpu_ms,flips,pacer_dropped_ms\n')
    for f in rec.frames:
        if args.from_ is not None and f[0] < args.from_:
            continue
        if args.to is not None and f[0] > args.to:
            break
        out.write('%d,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f\n' % f)
    if args.csv:
        out.close()
        print('%d frames -> %s' % (len(rec.frames), args.csv))


def default_runner():
    return os.environ.get('RUNNER') or os.path.join(zpaths.get('ZAXIS_MAIN'), 'out', 'build-coll', 'runtime',
                                                    'zx-runner')


def game_elf(game):
    if game == 'ai':
        return os.path.join(zpaths.get('AI_WORKSPACE'), 'recomp', 'disc-root', GAME_ELF['ai'])
    if game == 'dm2':
        return os.environ.get('ELF_DM2') or os.path.join(zpaths.get('DM2_WORKSPACE'), 'recomp', 'disc-root',
                                                         GAME_ELF['dm2'])
    if game == 'bmx':
        return os.environ.get('ELF_BMX') or os.path.join(zpaths.get('BMX_DATA'), 'disc', GAME_ELF['bmx'])
    sys.exit('unknown game %r' % game)


# What a replay sets itself (paths of the replay's own, the checks' switches) and never takes from the recording.
OWN = {'ZX_SAVES_DIR', 'ZX_GAMEPLAY_INI', 'ZX_CONTROLS_INI', 'ZX_SCREENSHOT_DIR', 'ZX_GS_PIPELINE_CACHE',
       'ZX_MODS_DIR', 'ZX_GAMEPAD_MAPPINGS', 'ZX_PAD_SCRIPT', 'ZX_PAD_SCRIPT_CLOCK', 'ZX_HOST_INPUT',
       'ZX_HOST_INPUT_SCRIPT', 'ZX_STATE_LOG_FILE', 'ZX_DUMP_PREFIX', 'ZX_GS_BACKEND', 'ZX_GS_SCALE',
       'ZX_GS_FULL_HEIGHT', 'ZX_RECORD', 'ZX_RECORD_CHECKSUMS', 'ZX_REPLAY', 'ZX_REPLAY_STOP'}
# Process switches of the recording that only change host time or what is shown; a replay sets its own.
HOST_ONLY = {'ZX_UNPACED', 'ZX_PERF_LOG', 'ZX_PERF_FRAMES', 'ZX_GS_NULL', 'ZX_DEBUG_UI', 'ZX_APP_GAME',
             'ZX_APP_SUPPORT_DIR', 'ZX_APP_LOG_DIR', 'ZX_MC_ROOT', 'ZX_VSYNC_PHASE', 'ZX_HEADLESS',
             'ZX_AUDIO'}
# Folders of bundled files: kept if they exist here, else looked up under --resources.
BUNDLED = {'ZX_XBOX_DIR': 'xbox', 'ZX_XBOX_CONVERTED': 'xbox-converted', 'ZX_ESTATE_DIR': 'estate',
           'ZX_DM2_XBOX_LEVELS_DIR': 'xbox-levels', 'ZX_BMX_RELEASES_DIR': 'releases', 'ZX_XBOX_PACK': 'xbox-pack'}


def prepare(rec, d, args, stop=None):
    """A run folder for a replay of rec and its environment."""
    for sub in ('home', 'cwd', 'app', 'shots', 'saves', 'dumps'):
        os.makedirs(os.path.join(d, sub), exist_ok=True)
    for sub in ('mc0', 'mc1', 'saves'):
        shutil.rmtree(os.path.join(d, sub), ignore_errors=True)
        os.makedirs(os.path.join(d, sub))
    gameplay = None
    for name, data in rec.files.items():
        label, _, rest = name.partition('/')
        target = {'card1': 'mc0', 'card2': 'mc1', 'saves': 'saves'}.get(label)
        if name == 'gameplay.ini':
            gameplay = os.path.join(d, 'gameplay.ini')
            path = gameplay
        elif target:
            path = os.path.join(d, target, rest)
        else:
            continue
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, 'wb') as f:
            f.write(data)
    env = dict(os.environ)
    for k in list(env):
        if k.startswith('ZX_') or k.startswith('ZAXIS_'):
            del env[k]
    env['ZX_AUDIO'] = os.environ.get('ZX_AUDIO', 'off')  # the shared environment's (zpaths: no sound)
    warnings = []
    for k, v in rec.env_values().items():
        if k not in HOST_ONLY and k not in OWN:
            env[k] = v
    for k, v in rec.boot_values().items():
        if k in OWN:
            continue
        if k in BUNDLED and not os.path.isdir(v):
            local = os.path.join(args.resources, BUNDLED[k]) if args.resources else ''
            if local and os.path.isdir(local):
                v = local
            else:
                warnings.append('%s: %s is not here (--resources DIR: the app\'s Resources/games/%s)' % (k, v, rec.game))
                continue
        env[k] = v
    env.update({
        'HOME': os.path.join(d, 'home'), 'ZX_APP_SUPPORT_DIR': os.path.join(d, 'app'),
        'ZX_APP_LOG_DIR': os.path.join(d, 'app'), 'ZX_SCREENSHOT_DIR': os.path.join(d, 'shots'),
        'ZX_DEBUG_UI': '0', 'ZX_MC_ROOT': os.path.join(d, 'mc0'), 'ZX_SAVES_DIR': os.path.join(d, 'saves'),
        'ZX_MODS_DIR': os.path.join(d, 'mods'), 'ZX_CONTROLS_INI': os.path.join(d, 'controls.ini'),
        'ZX_UNPACED': '1', 'ZX_HOST_INPUT': '0', 'ZX_REPLAY': os.path.abspath(rec.path),
        'ZX_GS_BACKEND': 'cpu', 'ZX_DUMP_PREFIX': os.path.join(d, 'dumps', 'ram'),
    })
    if gameplay:
        env['ZX_GAMEPLAY_INI'] = gameplay
    if not getattr(args, 'picture', False):
        env['ZX_GS_NULL'] = '1'
    if stop:
        env['ZX_REPLAY_STOP'] = str(stop)
    if getattr(args, 'record', False):
        env['ZX_RECORD'] = os.path.join(d, 'recorded')
    readable = getattr(args, 'readable', None)
    if readable is None:
        readable, why = recording_readable(rec)
        if readable is None:
            warnings.append('readable: %s: the runner\'s own set (the boot\'s timing may part from the recording\'s)'
                            % why)
        else:
            print('readable: %s' % why)
    elif readable == 'runner':
        readable = None  # (the recording's own ZX_READABLE, if it had one, on the runner's statuses, as above)
    if readable:
        env['ZX_READABLE'] = readable
    for kv in getattr(args, 'env', None) or []:
        k, _, v = kv.partition('=')
        env[k] = v
    if getattr(args, 'from_', None) is not None:
        # A window: every tracer's and profiler's start at the window's first vsync.
        for k in list(env):
            if k in ('ZX_FNREC', 'ZX_FNPROF', 'ZX_FNCYCLES') and k + '_FROM' not in env:
                env[k + '_FROM'] = str(args.from_)
    return env, warnings


def app_log(d):
    """The app's own log in a replay's folder (ZX_APP_LOG_DIR is its app folder): the newest."""
    logs = glob.glob(os.path.join(d, 'app', '**', '*.log'), recursive=True)
    return max(logs, key=os.path.getmtime) if logs else None


def launch(rec, env, d, name, runner, max_secs, watch=None):
    """Runs the replay: a runner given the game's ELF, or, for a recording the app made, the app's front end
    (--game GAME, the disc folder beside that ELF), which writes its own log and goes back to its menu when the
    game stops: it is stopped at the replay's summary. watch(): called each second while it runs, and once after."""
    log = os.path.join(d, name + '.log')
    elf = game_elf(rec.game)
    t0 = time.time()
    if rec.from_app:
        env = dict(env, ZX_APP_DISC_DIR=os.path.dirname(elf), ZX_HEADLESS=env.get('ZX_HEADLESS', '1'))
        for old in glob.glob(os.path.join(d, 'app', '**', '*.log'), recursive=True):
            os.remove(old)
        command = [runner, '--game', rec.game]
    else:
        command = [runner, elf]
    why = None
    with open(log, 'w') as out:
        proc = subprocess.Popen(command, cwd=os.path.join(d, 'cwd'), env=env, stdout=out, stderr=subprocess.STDOUT)
        while why is None:
            try:
                rc = proc.wait(timeout=1)
                why = 'exited rc=%d' % rc
                break
            except subprocess.TimeoutExpired:
                pass
            if watch:
                watch()
            if time.time() - t0 >= max_secs:
                why = 'stopped at the time limit'
            elif rec.from_app:
                found = app_log(d)
                if found and '[replay] summary:' in open(found, errors='replace').read():
                    time.sleep(1)  # the end record and the log's last lines
                    why = 'stopped at the replay\'s end (the app back at its menu)'
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()
    if watch:
        watch()
    if rec.from_app:
        found = app_log(d)
        if found:
            # One log for the verdict: the app's, after the process's own output.
            with open(log, 'a') as out, open(found, errors='replace') as inp:
                out.write('--- the app\'s log, %s\n' % found)
                shutil.copyfileobj(inp, out)
    return log, why, time.time() - t0


def verdict(log):
    summary, differs, first_misplaced, stop = None, [], None, None
    with open(log, errors='replace') as f:
        for line in f:
            if line.startswith('[replay] summary:'):
                summary = line.strip()
            elif line.startswith('[replay] state at vsync') and 'DIFFERS' in line:
                differs.append(line.strip())
            elif 'recorded at vsync' in line and line.startswith('[replay]') and first_misplaced is None:
                first_misplaced = line.strip()
            elif line.startswith('[replay] reached vsync'):
                stop = line.strip()
    return summary, differs, first_misplaced, stop


# ---- an AI recording from before the settings pages' room (b128) --------------------------------------------------

# AI's settings pages (the app's Settings page, Are You Sure, the pause menu's own items) take room in zgui's heap:
# 64 KB at the top of extended memory, kept out of every other heap (the runtime's zge_host::extendHeap and
# PS2Machine::reserveExtendedTop), linked after the heap's own sentinel. A build before b128 (hang1) had no room:
# zgui's Heap.last was the heap's own sentinel and that sentinel's HeapBlock.next 0, and the room's 64 KB were the
# empty memory they are at power-on. Nothing else differs: every other heap sits where it did (tools32: one recording
# replayed on its own build and on the fixed build, RAM dumped). A recording made before b128 carries checksums of a
# RAM without the room: its replay on a build with it (`run`) is compared with those two words and the room as its
# build had them, and every megabyte of the 64 MB the checksums cover is compared.
SETTINGS_ROOM_COMMIT = '5b7c29cca34e78bcee3ae9bca5ff603ce9d389cf'  # hang1's first: zgui's heap given the pages' room
LEGACY_RAM = 32 << 20     # the PS2's own RAM (runtime/memory.h PS2_LEGACY_RAM_SIZE); the room is above it
CHECKSUM_BLOCK = 1 << 20  # a megabyte per checksum (play_recording.cpp kChecksumBlock; the header's checksum_block)
# zge::Heap and zge::HeapBlock (engine/zge/memory/heaps.h), as AI has them
HEAP_LAST = 0x04
BLOCK_NEXT, BLOCK_PREV = 0x00, 0x04
BLOCK_HEADER = 0x10


def block_checksum(data):
    """One megabyte's checksum as play_recording.cpp's blockChecksum takes it: four multiply-xor lanes over 64-bit
    words, folded to 32 bits."""
    mask = (1 << 64) - 1
    mul = 0xFF51AFD7ED558CCD
    words = struct.unpack('<%dQ' % (len(data) // 8), data)
    l0, l1, l2, l3 = 0x9E3779B97F4A7C15, 0xC2B2AE3D27D4EB4F, 0x165667B19E3779F9, 0x27D4EB2F165667C5
    for i in range(0, len(words) - 3, 4):
        x = ((l0 ^ words[i]) * mul) & mask
        l0 = x ^ (x >> 29)
        x = ((l1 ^ words[i + 1]) * mul) & mask
        l1 = x ^ (x >> 29)
        x = ((l2 ^ words[i + 2]) * mul) & mask
        l2 = x ^ (x >> 29)
        x = ((l3 ^ words[i + 3]) * mul) & mask
        l3 = x ^ (x >> 29)
    h = (l0 ^ (l1 * 3) ^ (l2 * 5) ^ (l3 * 7) ^ len(data)) & mask
    h ^= h >> 33
    h = (h * 0xC4CEB9FE1A85EC53) & mask
    h ^= h >> 33
    return (h ^ (h >> 32)) & 0xFFFFFFFF


def commit_has(commit, ancestor):
    """Whether a z-axis commit (a recording's commit=, a '+changes' after it ignored) has ancestor in it: True, False,
    or None when the main checkout does not know the commit."""
    sha = commit.split('+')[0]
    if not sha or sha == 'unknown':
        return None
    p = subprocess.run(['git', '-C', zpaths.get('ZAXIS_MAIN'), 'merge-base', '--is-ancestor', ancestor, sha],
                       capture_output=True)
    return {0: True, 1: False}.get(p.returncode)


def build_registrations(commit):
    """The readable registrations of a z-axis commit, [{game, addr, name, status}] as that commit's own
    readable_digest.py --json reads its readable code (statuses from the source, an _ONLY row's from its
    game's symbols_status.tsv), cached per commit beside the replays; None when the main checkout does not know it."""
    main = zpaths.get('ZAXIS_MAIN')
    p = subprocess.run(['git', '-C', main, 'rev-parse', '--verify', '--quiet', commit + '^{commit}'],
                       capture_output=True, text=True)
    if p.returncode != 0:
        return None
    sha = p.stdout.strip()
    cache = os.path.join(run_dir('.readable-sets'), sha + '.json')
    if os.path.exists(cache):
        with open(cache) as f:
            return json.load(f)
    tree = cache + '.tree'
    shutil.rmtree(tree, ignore_errors=True)
    os.makedirs(tree)
    try:
        # (a commit from before layout1 has its readable code in runtime/src/game and its tools in runtime/tools)
        old = subprocess.run(['git', '-C', main, 'cat-file', '-e', sha + ':runtime/src/game'],
                             capture_output=True).returncode == 0
        folders = ['runtime/src/game', 'runtime/tools'] if old else [
            'engine/zge', 'engine/host', 'ps2/ee', 'ps2/sce', 'games', 'tools/readable', 'tools/checks/zlayout.py']
        archive = subprocess.Popen(['git', '-C', main, 'archive', sha] + folders, stdout=subprocess.PIPE)
        subprocess.run(['tar', '-x', '-C', tree], stdin=archive.stdout, check=True)
        archive.stdout.close()
        if archive.wait() != 0:
            return None
        command = ([os.path.join(tree, 'runtime', 'tools', 'readable_digest.py'), '--json',
                    os.path.join(tree, 'runtime', 'src', 'game')] if old else
                   [os.path.join(tree, 'tools', 'readable', 'readable_digest.py'), '--json', '--tree', tree])
        out = subprocess.run([sys.executable, '-B'] + command, cwd=tree, capture_output=True, text=True)
        if out.returncode != 0:
            return None
        rows = [{k: r[k] for k in ('game', 'addr', 'name', 'status')} for r in json.loads(out.stdout)]
    finally:
        shutil.rmtree(tree, ignore_errors=True)
    with open(cache + '.part', 'w') as f:
        json.dump(rows, f)
    os.replace(cache + '.part', cache)
    return rows


def recording_readable(rec):
    """ZX_READABLE for a replay of rec: the readable functions its build ran, by address ('off,+ADDR,...'), and
    what it is; or (None, why not). The readable code charges none of the original's loop cycles (32 at each branch
    back), so EE time, which decides the vsync each read falls in, depends on which functions are on: a replay on a
    runner that promoted some since (b131's sixteen: strcpy alone moved the boot's prompt-generation read from vsync 4
    to 3) parts from the recording on its first such read, though no function's result changed. The set is the
    commit's Verified rows of the recording's game with the recording's own ZX_READABLE applied as that build
    applied it (ps2/ee/readable.cpp applySwitch). A function the runner has not got stays off (the runtime names it)."""
    commit = rec.header.get('commit', '').split('+')[0]
    if not commit or commit == 'unknown':
        return None, 'the recording names no build'
    rows = build_registrations(commit)
    if rows is None:
        return None, 'the recording\'s build %s is not in the main checkout' % commit[:9]
    rows = [r for r in rows if r['game'] == rec.game]
    if not rows:
        return None, 'the recording\'s build %s registers no %s function' % (commit[:9], rec.game)
    rank = {'Draft': 0, 'Tested': 1, 'Verified': 2}
    on = {r['addr'] for r in rows if r['status'] == 'Verified'}
    spec = rec.boot_values().get('ZX_READABLE') or rec.env_values().get('ZX_READABLE') or ''
    by_key = {}
    for r in rows:
        by_key.setdefault(r['name'], r['addr'])
        by_key.setdefault(r['addr'].lower(), r['addr'])
    for item in (i.strip() for i in spec.split(',')):
        if not item:
            continue
        if item in ('on', 'verified', 'default'):
            on = {r['addr'] for r in rows if r['status'] == 'Verified'}
        elif item == 'off':
            on = set()
        elif item == 'tested':
            on = {r['addr'] for r in rows if rank.get(r['status'], 0) >= rank['Tested']}
        elif item == 'all':
            on = {r['addr'] for r in rows}
        else:
            key = item[1:] if item[0] in '+-' else item
            if ':' in key:  # (`dm2:NAME`: another game's, which this game's set does not hold)
                prefix, key = key.split(':', 1)
                if prefix != rec.game:
                    continue
            addr = by_key.get(key) or by_key.get(key.lower())
            if addr is None and key.lower().startswith('0x'):
                addr = by_key.get('0x%08x' % int(key, 16))
            if addr is not None:
                (on.discard if item[0] == '-' else on.add)(addr)
    if not on:
        return 'off', 'the recording\'s build %s ran none readable' % commit[:9]
    return ('off,' + ','.join('+' + a for a in sorted(on)),
            'the %d functions the recording\'s build %s ran (%s%s)' % (
                len(on), commit[:9], 'its Verified rows', ', its ZX_READABLE=%s' % spec if spec else ''))


def game_symbol(name, game):
    """A data symbol's address in the game (the main checkout's tools/readable/readable_symbols.tsv)."""
    path = zpaths.tree_path(zpaths.get('ZAXIS_MAIN'), 'tools', 'readable', 'readable_symbols.tsv')
    with open(path, encoding='utf-8') as f:
        for line in f:
            kind, sym, address, _, of = (line.rstrip('\n').split('\t') + [''] * 5)[:5]
            if kind == 'data' and sym == name and of in ('', game) and (of or game == 'ai'):
                return int(address, 16)
    sys.exit('%s: no data symbol %s for %s' % (path, name, game))


class SettingsRoomView:
    """A replay of an AI recording from before b128's settings room, on a build with it: the RAM dumped at each of
    the recording's checksums (ZX_DUMP_EXTENDED: the 64 MB the checksums cover), zgui's two heap links put back and
    the room cleared as the recording's build had them, the megabytes they are in checksummed here; every other
    megabyte compared as the runtime took it (the replay's own recording)."""

    @staticmethod
    def needed(rec):
        """Why rec needs the view (None: it does not), or a warning when it cannot be compared word for word."""
        if rec.game != 'ai' or rec.boot_values().get('ZX_GAME_SETTINGS_MENUS') != '1':
            return None    # (the room comes with the settings pages only)
        commit = rec.header.get('commit', '')
        has = commit_has(commit, SETTINGS_ROOM_COMMIT)
        if has is None:
            return 'warning: the recording\'s build %s is not in the main checkout: compared as it is' % commit
        if has:
            return None
        return 'the recording\'s build %s has no settings room (before b128)' % commit

    def __init__(self, rec, d, stop):
        self.rec, self.dumps = rec, os.path.join(d, 'dumps')
        self.heap_pointer = game_symbol('g_heap', 'ai')
        self.vsyncs = sorted(v for v in rec.checksums if stop is None or v <= stop)
        self.sums = {}       # vsync -> {megabyte: checksum of the view}
        self.links = None    # (Heap.last's address, the heap's own sentinel, the room's start and end)
        self.missing = []    # recorded checksums with no dump (the replay stopped before)

    def env(self, recorded):
        every = self.rec.header.get('checksum_every', '60')
        return {'ZX_DUMP_WHEN': ','.join('c%d:tick>=%d' % (v, v) for v in self.vsyncs),
                'ZX_DUMP_EXTENDED': '1', 'ZX_RECORD': recorded, 'ZX_RECORD_CHECKSUMS': every}

    def watch(self):
        """Each whole dump taken in and removed (64 MB a dump: a long session's would not fit on the disk)."""
        for v in self.vsyncs:
            if v in self.sums:
                continue
            path = os.path.join(self.dumps, 'ram-c%d.ram' % v)
            if not os.path.isfile(path) or os.path.getsize(path) < 2 * LEGACY_RAM:
                continue
            with open(path, 'rb') as f:
                ram = bytearray(f.read())
            self.sums[v] = self.view(ram)
            os.remove(path)
            sp = path[:-len('.ram')] + '.sp'
            if os.path.isfile(sp):
                os.remove(sp)

    def view(self, ram):
        word = lambda a: struct.unpack_from('<I', ram, a & 0x1FFFFFFF)[0]
        heap = word(self.heap_pointer)
        if not heap or word(heap + HEAP_LAST) < LEGACY_RAM:
            return {}      # (zgui not up yet, or no room linked: the RAM as it is)
        last = word(heap + HEAP_LAST)          # the room's sentinel
        start = last                           # the room's first block: back from its sentinel to the 32 MB
        while LEGACY_RAM <= word(start + BLOCK_PREV) < len(ram):
            start = word(start + BLOCK_PREV)
        own = word(start + BLOCK_PREV)         # the heap's own sentinel
        if own >= LEGACY_RAM or word(own + BLOCK_NEXT) != start:
            sys.exit('settings room: zgui\'s heap at %#x is not linked as the room makes it (Heap.last %#x, its '
                     'first block %#x after %#x)' % (heap, last, start, own))
        end = (last & 0x1FFFFFFF) + BLOCK_HEADER
        if self.links is None:
            self.links = (heap + HEAP_LAST, own, start, end)
        before = {heap + HEAP_LAST: own, own + BLOCK_NEXT: 0}
        for address, value in before.items():
            struct.pack_into('<I', ram, address & 0x1FFFFFFF, value)
        ram[start & 0x1FFFFFFF:end] = bytes(end - (start & 0x1FFFFFFF))
        touched = {(a & 0x1FFFFFFF) // CHECKSUM_BLOCK for a in before}
        touched |= set(range((start & 0x1FFFFFFF) // CHECKSUM_BLOCK, (end - 1) // CHECKSUM_BLOCK + 1))
        return {mb: block_checksum(bytes(ram[mb * CHECKSUM_BLOCK:(mb + 1) * CHECKSUM_BLOCK])) for mb in touched}

    def compare(self, made):
        """The recording's checksums against the replay's own (made) with the view's megabytes in place: (matched,
        differed, the first difference's text, megabytes compared)."""
        matched, differed, first, megabytes = 0, 0, None, 0
        for v in self.vsyncs:
            if v not in made.checksums:
                self.missing.append(v)
                continue
            got, want = list(made.checksums[v]), list(self.rec.checksums[v])
            for mb, s in self.sums.get(v, {}).items():
                got[mb] = s
            megabytes += len(want)
            if got == want:
                matched += 1
            else:
                differed += 1
                if first is None:
                    first = 'vsync %d, MB %s' % (v, ','.join(str(i) for i in range(len(want))
                                                            if i >= len(got) or got[i] != want[i]))
        return matched, differed, first, megabytes

    def report(self, made):
        short = glob.glob(os.path.join(self.dumps, 'ram-c*.ram'))
        if short and not self.sums:
            print('  settings room: the runner\'s dumps hold the 32 MB only (a build before ZX_DUMP_EXTENDED): '
                  'nothing compared')
            for path in short:
                os.remove(path)
                if os.path.isfile(path[:-len('.ram')] + '.sp'):
                    os.remove(path[:-len('.ram')] + '.sp')
            return False
        matched, differed, first, megabytes = self.compare(made)
        if self.links:
            print('  settings room: Heap.last at %#x and the heap\'s own sentinel %#x (its next) put back as before '
                  'b128, the room %#x-%#x as the empty memory it was' % self.links)
        print('  settings room: %d checksums matched, %d differed%s (of %d to the stop; %d megabyte checksums '
              'compared, the others as the runtime took them)' % (
                  matched, differed, ' (first at %s)' % first if first else '', len(self.vsyncs), megabytes))
        if self.missing:
            print('  settings room: %d of the recording\'s checksums are past the replay\'s end' % len(self.missing))
        return differed == 0 and matched > 0


def run_dir(name):
    return os.path.join(os.environ.get('RUN_DIR') or os.path.join(zpaths.ZAXIS_SCRATCH, 'replays'), name)


def chunk_stop(rec, args):
    if getattr(args, 'moment', None) is not None:
        found = moments(rec)
        if not 0 <= args.moment < len(found):
            sys.exit('no moment %d (replay.py moments %s)' % (args.moment, rec.path))
        m = found[args.moment]
        print('moment %d: vsync %d %s: %s' % (args.moment, m['vsync'], m['kind'], m['detail']))
        return m['vsync'] + (args.after or 0)
    return args.to


def cmd_run(args):
    rec = Recording(args.file)
    stop = chunk_stop(rec, args)
    name = args.name or '%s-%s' % (os.path.splitext(os.path.basename(args.file))[0], stop or 'end')
    d = run_dir(name)
    env, warnings = prepare(rec, d, args, stop)
    for w in warnings:
        print('warning: ' + w)
    room, why_room = None, (None if args.as_is else SettingsRoomView.needed(rec))
    if why_room and not why_room.startswith('warning: '):
        room = SettingsRoomView(rec, d, stop)
        shutil.rmtree(room.dumps, ignore_errors=True)
        os.makedirs(room.dumps)
        env.update(room.env(os.path.join(d, 'recorded')))
        print('%s: compared with zgui\'s heap links as its build had them' % why_room)
    elif why_room:
        print(why_room)
    log, why, secs = launch(rec, env, d, name, args.runner or default_runner(), args.max_secs,
                            watch=room.watch if room else None)
    summary, differs, misplaced, reached = verdict(log)
    print('%s after %.0f s: %s' % (why, secs, log))
    for line in differs[:5] if not room else []:
        print('  ' + line)
    if misplaced:
        print('  ' + misplaced)
    print('  ' + (reached or '(the replay did not reach its stop)'))
    print('  ' + (summary or '(no replay summary: the run did not end cleanly)'))
    if args.record:
        made = sorted(glob.glob(os.path.join(d, 'recorded', '*.zrec')))
        if made:
            print('  the replay\'s own recording: %s' % made[-1])
    if room:
        made = sorted(glob.glob(os.path.join(d, 'recorded', '*.zrec')), key=os.path.getmtime)
        same = room.report(Recording(made[-1])) if made else False
        if not made:
            print('  settings room: the replay made no recording of its own: nothing compared')
        ok = same and summary and ', 0 inputs at another vsync' in summary
        return 0 if ok and reached else 1
    ok = summary and ' 0 differed' in summary and ', 0 inputs at another vsync' in summary
    return 0 if ok and reached else 1


def cmd_twice(args):
    rec = Recording(args.file)
    ats = [int(x) for x in args.at.split(',')]
    stop = max(ats) + 1
    dumps = []
    for side in ('a', 'b'):
        name = '%s-twice-%s' % (os.path.splitext(os.path.basename(args.file))[0], side)
        d = run_dir(name)
        shutil.rmtree(os.path.join(d, 'dumps'), ignore_errors=True)
        args.env = (args.env or []) + ['ZX_DUMP_WHEN=' + ','.join('v%d:tick>=%d' % (v, v) for v in ats)]
        env, warnings = prepare(rec, d, args, stop)
        args.env = args.env[:-1]
        log, why, secs = launch(rec, env, d, name, args.runner or default_runner(), args.max_secs)
        summary, differs, misplaced, reached = verdict(log)
        print('%s: %s after %.0f s; %s' % (side, why, secs, summary))
        dumps.append(os.path.join(d, 'dumps'))
    same = True
    for v in ats:
        files = [sorted(glob.glob(os.path.join(x, '*-v%d*.ram' % v))) for x in dumps]
        if not files[0] or not files[1]:
            print('vsync %d: a dump is missing (%s)' % (v, files))
            same = False
            continue
        a, b = (open(f[0], 'rb').read() for f in files)
        words = sum(1 for i in range(0, min(len(a), len(b)), 4) if a[i:i + 4] != b[i:i + 4])
        print('vsync %d: RAM %d bytes, %s' % (v, len(a), 'the same' if a == b else '%d words differ' % words))
        same = same and a == b
    return 0 if same else 1


def cmd_compare(args):
    a, b = Recording(args.a), Recording(args.b)
    common = sorted(set(a.checksums) & set(b.checksums))
    differ = [v for v in common if a.checksums[v] != b.checksums[v]]
    print('%d checksums in both (to vsync %d); %d differ%s' % (
        len(common), common[-1] if common else 0, len(differ),
        (' (first at vsync %d, megabytes %s)' % (differ[0], [i for i, (x, y) in enumerate(
            zip(a.checksums[differ[0]], b.checksums[differ[0]])) if x != y])) if differ else ''))
    # Inputs: the same values at the same reads.
    def stream(rec):
        return [(c, k, r, val) for c, k, r, v, val in rec.inputs]
    sa, sb = stream(a), stream(b)
    n = min(len(sa), len(sb))
    first = next((i for i in range(n) if sa[i] != sb[i]), None)
    print('input changes: %d and %d; %s' % (len(sa), len(sb), 'the same' if first is None and len(sa) == len(sb)
                                            else 'first difference at change %s' % (first if first is not None else n)))
    # Telemetry on the vsync index: the game thread's CPU per second of vsyncs in each (a replay is unpaced, so
    # its frame times differ; where the game works hardest does not).
    def per_second(rec):
        out = {}
        for f in rec.frames:
            out[f[0] // 60] = out.get(f[0] // 60, 0.0) + f[2]
        return out
    pa, pb = per_second(a), per_second(b)
    keys = sorted(set(pa) & set(pb))
    if len(keys) > 2:
        xs, ys = [pa[k] for k in keys], [pb[k] for k in keys]
        mx, my = sum(xs) / len(xs), sum(ys) / len(ys)
        cov = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
        var = (sum((x - mx) ** 2 for x in xs) * sum((y - my) ** 2 for y in ys)) ** 0.5
        heavy = lambda d: sorted(k * 60 for k in sorted(d, key=lambda k: -d[k])[:5])
        print('telemetry: %d seconds of vsyncs in both; game-thread CPU per second correlates %.3f; heaviest at vsync '
              '%s and %s' % (len(keys), cov / var if var else 0.0, heavy(pa), heavy(pb)))
    return 0 if not differ else 1


def cmd_route(args):
    rec = Recording(args.file)
    os.makedirs(args.out, exist_ok=True)
    args.env = (args.env or []) + ['ZX_FNREC=all', 'ZX_FNREC_DIR=' + os.path.abspath(args.out)]
    args.readable = 'off'
    args.record = False
    args.picture = False
    args.moment = None
    name = 'route-' + os.path.splitext(os.path.basename(args.file))[0]
    d = run_dir(name)
    env, warnings = prepare(rec, d, args, args.to)
    for w in warnings:
        print('warning: ' + w)
    log, why, secs = launch(rec, env, d, name, args.runner or default_runner(), args.max_secs)
    summary, differs, misplaced, reached = verdict(log)
    files = glob.glob(os.path.join(args.out, '**', '*.fnrec'), recursive=True)
    print('%s after %.0f s: %d recorded functions in %s; %s' % (why, secs, len(files), args.out, summary))
    print('(readable off: where it charges other EE cycles than the recorded run the state may part from the '
          'recording; the summary above says whether it stayed the same)')
    return 0


def cmd_script(args):
    rec = Recording(args.file)
    items, held, sticks, since = [], {}, [0x80] * 4, [0] * 4
    names = ('RX', 'RY', 'LX', 'LY')
    pads = [(v, val) for c, k, r, v, val in rec.inputs if c == 0]
    # The script's clock starts at the first pad read (PadScript: its origin), not at power-on.
    origin = pads[0][0] if pads else 0
    pads = [(v - origin, val) for v, val in pads]
    for vsync, value in pads + [(rec.last_vsync() - origin, None)]:
        if value is None:
            mask, now = 0, [0x80] * 4
        else:
            raw = value[0] | (value[1] << 8)
            mask = ~raw & 0xFFFF
            now = list(value[2:6])
        for bit, name in PAD_BUTTONS.items():
            if mask & bit and bit not in held:
                held[bit] = vsync
            elif not mask & bit and bit in held:
                start = held.pop(bit)
                items.append((start, '%dv:%s:%dv' % (start, name, max(1, vsync - start))))
        for i in range(4):
            if now[i] != sticks[i]:
                if sticks[i] != 0x80:
                    items.append((since[i], '%dv:%s=%.4f:%dv' % (since[i], names[i], (sticks[i] - 128) / 127.5,
                                                                max(1, vsync - since[i]))))
                sticks[i], since[i] = now[i], vsync
    items.sort()
    with open(args.out, 'w') as f:
        f.write(','.join(text for _, text in items) + '\n')
    print('%d entries -> %s (ZX_PAD_SCRIPT=@%s ZX_PAD_SCRIPT_CLOCK=vsync)' % (len(items), args.out, args.out))


def varint(n):
    out = bytearray()
    while True:
        b, n = n & 0x7F, n >> 7
        if not n:
            out.append(b)
            return bytes(out)
        out.append(b | 0x80)


def cmd_extend(args):
    rec = Recording(args.file)
    if rec.end:
        sys.exit('%s has its end record (%s): nothing was lost' % (args.file, rec.end[1]))
    pads = [(r, v, val) for c, k, r, v, val in rec.inputs if c == 0 and k == 0]
    if not pads:
        sys.exit('%s: no pad reads' % args.file)
    read0, vsync0, last = pads[-1]
    idle = bytearray(last)
    idle[0:2] = b'\xff\xff'
    idle[6:] = bytes(len(idle) - 6)  # (no button held, no pressure; the sticks as last read)
    pressure = {'RIGHT': 6, 'LEFT': 7, 'UP': 8, 'DOWN': 9, 'TRIANGLE': 10, 'CIRCLE': 11, 'CROSS': 12, 'SQUARE': 13,
                'L1': 14, 'R1': 15, 'L2': 16, 'R2': 17}
    bits = {name: bit for bit, name in PAD_BUTTONS.items()}
    changes = []
    for spec in args.press:
        name, _, rest = spec.partition('@')
        at, _, hold = rest.partition(':')
        at, hold = int(at), int(hold or 4)
        if name not in bits or at <= vsync0:
            sys.exit('%s: a button of %s, after vsync %d' % (spec, '/'.join(sorted(bits)), vsync0))
        value = bytearray(idle)
        raw = 0xFFFF & ~bits[name]
        value[0], value[1] = raw & 0xFF, raw >> 8
        if name in pressure and pressure[name] < len(value):
            value[pressure[name]] = 0xFF
        changes += [(at, bytes(value)), (at + hold, bytes(idle))]
    out = bytearray(open(args.file, 'rb').read()[:rec.size - rec.cut_short_bytes])
    prev_read, prev = read0, last
    for at, value in sorted(changes):
        read = read0 + (at - vsync0)
        mask = sum(1 << i for i in range(min(len(value), 64)) if value[i] != prev[i])
        payload = varint(0) + varint(0) + varint(read - prev_read) + varint(at) + varint(len(value)) + varint(mask)
        payload += bytes(value[i] for i in range(len(value)) if (mask >> i) & 1)
        out += bytes([INPUT]) + varint(len(payload)) + payload
        prev_read, prev = read, value
    frame = b''.join(varint(x) for x in (args.to, 16667, 0, 0, 0, 0, 0, 0))
    out += bytes([FRAME]) + varint(len(frame)) + frame
    with open(args.out, 'wb') as f:
        f.write(out)
    print('%s: %d presses after vsync %d, to vsync %d (replay.py run %s --to %d)' % (
        args.out, len(args.press), vsync0, args.to, args.out, args.to))


def cmd_pull(args):
    dest = args.to or os.path.join(zpaths.ZAXIS_SCRATCH, 'recordings', 'thor')
    os.makedirs(dest, exist_ok=True)
    base = '/storage/emulated/0/Android/data/%s/files' % PACKAGE
    adb = ['adb', '-s', args.serial] if args.serial else ['adb']
    listing = subprocess.run(adb + ['shell', 'find', base, '-name', '*.zrec'], capture_output=True, text=True)
    files = [x.strip() for x in listing.stdout.splitlines() if x.strip().endswith('.zrec')]
    for remote in files:
        local = os.path.join(dest, os.path.basename(remote))
        if os.path.exists(local):
            continue
        subprocess.run(adb + ['pull', remote, local], check=False)
    print('%d recordings on the device, in %s' % (len(files), dest))


def cmd_list(args):
    folder = args.dir or os.path.expanduser('~/Library/Application Support/Z-Axis Collection')
    files = sorted(glob.glob(os.path.join(folder, '**', '*.zrec'), recursive=True))
    for path in files:
        rec = Recording(path)
        print('%s  %s  %7d vsyncs  %8d bytes  %s' % (os.path.relpath(path, folder), rec.game, rec.last_vsync(),
                                                    rec.size, rec.end[1] if rec.end else 'no end (crash or kill)'))
    print('%d recordings in %s' % (len(files), folder))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('info')
    p.add_argument('file')
    p = sub.add_parser('moments')
    p.add_argument('file')
    p.add_argument('--json', action='store_true')
    p = sub.add_parser('telemetry')
    p.add_argument('file')
    p.add_argument('--csv')
    p.add_argument('--from', dest='from_', type=int)
    p.add_argument('--to', type=int)

    def runner_args(p):
        p.add_argument('--runner')
        p.add_argument('--resources', help="the app's Resources/games/<id> (bundled files for a recording made "
                       'elsewhere)')
        p.add_argument('--env', action='append', help='K=V for the run (traces, profiles)')
        p.add_argument('--max-secs', type=int, default=1800)
        p.add_argument('--readable')

    p = sub.add_parser('run')
    p.add_argument('file')
    g = p.add_mutually_exclusive_group()
    g.add_argument('--to', type=int)
    g.add_argument('--moment', type=int)
    p.add_argument('--after', type=int, default=0, help='with --moment: vsyncs past the moment')
    p.add_argument('--from', dest='from_', type=int)
    p.add_argument('--name')
    p.add_argument('--record', action='store_true')
    p.add_argument('--picture', action='store_true')
    p.add_argument('--as-is', action='store_true', help='the runtime\'s own comparison only (no SettingsRoomView)')
    runner_args(p)
    p = sub.add_parser('twice')
    p.add_argument('file')
    p.add_argument('--at', required=True)
    runner_args(p)
    p = sub.add_parser('compare')
    p.add_argument('a')
    p.add_argument('b')
    p = sub.add_parser('route')
    p.add_argument('file')
    p.add_argument('out')
    p.add_argument('--to', type=int)
    runner_args(p)
    p = sub.add_parser('script')
    p.add_argument('file')
    p.add_argument('out')
    p = sub.add_parser('extend')
    p.add_argument('file')
    p.add_argument('out')
    p.add_argument('--to', type=int, required=True)
    p.add_argument('press', nargs='*')
    p = sub.add_parser('pull')
    p.add_argument('--serial', default=THOR)
    p.add_argument('--to')
    p = sub.add_parser('list')
    p.add_argument('dir', nargs='?')
    args = ap.parse_args()
    # (a command that writes: not from or into the main checkout, unless the orchestrator's scripts run it: zguard.py,
    # tools36. Its runs' folder (RUN_DIR), an out file or folder, --csv, pull's --to)
    if args.cmd not in ('info', 'moments', 'list', 'compare') and not (args.cmd == 'telemetry' and not args.csv):
        name = getattr(args, 'name', None) or 'x'
        zguard.refuse('replay.py', [run_dir(name) if args.cmd in ('run', 'twice', 'route') else None,
                                    getattr(args, 'out', None), getattr(args, 'csv', None),
                                    getattr(args, 'to', None) if args.cmd == 'pull' else None])
    return globals()['cmd_' + args.cmd](args) or 0


if __name__ == '__main__':
    sys.exit(main())
