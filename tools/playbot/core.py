#!/usr/bin/env python3
"""Closed-loop players for the Z-Axis games: the game-neutral part.

A player runs a native runner with ZX_PLAY_LINK (runtime/play_link.h): at every port-0 pad read
the game stops, the player reads guest memory, chooses the pad, and the game goes on. Nothing here knows
a game: dm2.py (and aggressive.py, playai's) give the table of what to read and how to play.

    Run(RunConfig(...))        a runner on a link, always with a stop condition, never outliving its player
    Pad                        one pad answer (buttons by name, sticks -1 .. 1)
    Recorder                   the answers as a ZX_PAD_SCRIPT (script time, so a run replays tick for tick)
    Stats                      named counters, sums and bests, printed as a row
    play(run, policy, ...)     the loop: run.read state, policy(state) -> Pad, record, answer
    digest / Trace             a per-tick record of state, to compare two runs tick for tick
    v3 helpers, angle_to, wrap steering geometry

policy is any callable: state -> Pad, or None to stop the run. Time limits are not optional: `play` needs
max_ticks, and RunConfig.max_secs is the wall limit. A game table gives `read(run) -> state`.
"""
import dataclasses
import hashlib
import json
import math
import os
import select
import signal
import struct
import subprocess
import sys
import time

BUTTONS = {'SELECT': 0x0001, 'L3': 0x0002, 'R3': 0x0004, 'START': 0x0008, 'UP': 0x0010, 'RIGHT': 0x0020,
           'DOWN': 0x0040, 'LEFT': 0x0080, 'L2': 0x0100, 'R2': 0x0200, 'L1': 0x0400, 'R1': 0x0800,
           'TRIANGLE': 0x1000, 'CIRCLE': 0x2000, 'CROSS': 0x4000, 'SQUARE': 0x8000}
AXES = ('RX', 'RY', 'LX', 'LY')
HEAD = struct.Struct('<cQqI')

# The players' own scratch folders (PLAYAI, PLAYDM2, BMXCORPUS, PLAYCOLL: each player's SCRATCH, its maps and runs)
PLAYER_SCRATCH = ('PLAYAI', 'PLAYDM2', 'BMXCORPUS', 'PLAYCOLL')
TOOLING = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))   # (tools/: zguard.py)


def refuse_main(outputs=()):
    """Stopped before anything is written when the working directory, a player's scratch folder or OUTPUTS is inside
    the main checkout, unless the orchestrator's merge and batch scripts run it (ZAXIS_IN_MAIN=1): the tooling's
    zguard.py (tools36: saves1's BMXCORPUS=playbmx made playbmx/ there from the Bash tool's start folder). Every
    player imports this module, so each entry point is guarded at its first line; Run checks its own folder too."""
    if not os.path.isfile(os.path.join(TOOLING, 'zguard.py')):
        return      # (a copy of the players without the tooling: no main checkout to guard)
    sys.path.insert(0, TOOLING)
    try:
        import zguard
    finally:
        sys.path.pop(0)
    zguard.refuse(os.path.basename(sys.argv[0] or 'playbot') or 'playbot',
                  [os.environ.get(k) for k in PLAYER_SCRATCH] + list(outputs))


refuse_main()


def stick_byte(v):
    """The byte a pad script gives for an axis value: runtime PadScript's axisByte."""
    return max(0, min(255, int(math.floor(128.0 + max(-1.0, min(1.0, v)) * 127.5 + 0.5))))


def stick_value(byte):
    """An axis value whose script byte is exactly `byte`."""
    return round((byte - 128) / 127.5, 4)


@dataclasses.dataclass(frozen=True)
class Pad:
    """A pad answer. Sticks left at None are not touched (the script's, or centre)."""
    buttons: frozenset = frozenset()
    lx: float = None
    ly: float = None
    rx: float = None
    ry: float = None

    def wire(self, skip=0):
        mask = 0
        for b in self.buttons:
            mask |= BUTTONS[b]
        axes, vals = 0, [128, 128, 128, 128]
        for i, v in enumerate((self.rx, self.ry, self.lx, self.ly)):
            if v is not None:
                axes |= 1 << i
                vals[i] = stick_byte(v)
        return struct.pack('<cHB4BI', b'G', mask, axes, *vals, skip)


NOTHING = Pad()


def pad(*buttons, lx=None, ly=None, rx=None, ry=None):
    return Pad(frozenset(buttons), lx, ly, rx, ry)


@dataclasses.dataclass
class RunConfig:
    name: str                        # folder under root
    runner: str
    elf: str
    root: str                        # runs go in root/name: scratch HOME, card, cwd, app, shots, link
    script: str = ''                 # ZX_PAD_SCRIPT: the way to the level (menus)
    env: dict = dataclasses.field(default_factory=dict)  # None: unset
    max_secs: float = 600.0
    gs: bool = False                 # draw (CPU GS), for pictures; else the null GS
    keep_card: str = None            # a card folder to start from, else a fresh one


class RunnerGone(Exception):
    pass


class Run:
    """A runner on a link. `with Run(cfg) as run:` guarantees the runner is gone at the end."""

    def __init__(self, cfg):
        self.cfg = cfg
        self.dir = os.path.join(cfg.root, cfg.name)
        self.link = os.path.join(self.dir, 'link')
        self.proc = None
        self.up = self.down = None
        self.tick = self.script_time = self.reads = 0
        self.t0 = time.time()

    def __enter__(self):
        cfg = self.cfg
        refuse_main([self.dir])
        for sub in ('home', 'cwd', 'app', 'shots', 'mc0', 'mc1', 'link', 'ctl'):
            os.makedirs(os.path.join(self.dir, sub), exist_ok=True)
        for f in ('up', 'down'):
            p = os.path.join(self.link, f)
            if os.path.exists(p):
                os.unlink(p)
            os.mkfifo(p)
        for f in ('cmd', 'out'):
            open(os.path.join(self.dir, 'ctl', f), 'w').close()
        env = dict(os.environ)
        d = self.dir
        env.update({'ZX_UNPACED': '1', 'ZX_DEBUG_UI': '0', 'ZX_GS_BACKEND': 'cpu', 'ZX_HOST_INPUT': '0',
                    'ZX_PAD_SCRIPT_CLOCK': 'vsync', 'HOME': os.path.join(d, 'home'),
                    'ZX_APP_SUPPORT_DIR': os.path.join(d, 'app'), 'ZX_APP_LOG_DIR': os.path.join(d, 'app'),
                    'ZX_SCREENSHOT_DIR': os.path.join(d, 'shots'), 'ZX_MC_ROOT': os.path.join(d, 'mc0'),
                    'ZX_CONTROL': os.path.join(d, 'ctl')})
        # (a park's memory run, PARK_MEMORY: the runtime's heap counters per level in its log, memuse.heap)
        if os.environ.get('PARK_MEMORY'):
            env.setdefault('ZX_TRACE_HEAP', '1')
        if cfg.env.get('ZX_PLAY_LINK', 1) is not None:
            env['ZX_PLAY_LINK'] = self.link
        if not cfg.gs:
            env.update({'ZX_GS_NULL': '1', 'ZX_VU1_SKIP': '1'})
        if cfg.script:
            path = os.path.join(d, 'script.txt')
            with open(path, 'w') as f:
                f.write(cfg.script)
            env['ZX_PAD_SCRIPT'] = '@' + path
        for k, v in cfg.env.items():
            if v is None:
                env.pop(k, None)
            else:
                env[k] = str(v)
        # PLAYBOT_ENV="K=V;K=;...": over any player's own environment (an empty V unsets K), e.g. ZX_READABLE= for
        # the readable defaults where a player sets off, or ZX_SAVES= for the native saves
        for kv in filter(None, os.environ.get('PLAYBOT_ENV', '').split(';')):
            k, v = kv.split('=', 1)
            if v:
                env[k] = v
            else:
                env.pop(k, None)
        self.log = open(os.path.join(d, 'run.log'), 'w')
        self.proc = subprocess.Popen([cfg.runner, cfg.elf], cwd=os.path.join(d, 'cwd'), env=env, stdout=self.log,
                                     stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        self.t0 = time.time()
        if 'ZX_PLAY_LINK' in env:
            self.up = os.open(os.path.join(self.link, 'up'), os.O_RDONLY | os.O_NONBLOCK)
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def close(self):
        """Stop the runner: the link closing ends it; by PID if it does not go."""
        if self.down is not None:
            os.close(self.down)
            self.down = None
        if self.up is not None:
            os.close(self.up)
            self.up = None
        if self.proc is None:
            return
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.send_signal(signal.SIGTERM)
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
        self.proc = None
        self.log.close()

    def _read(self, n):
        """n bytes from the game, with the wall limit and the runner's death noticed."""
        data = bytearray()
        while len(data) < n:
            if not select.select([self.up], [], [], 2.0)[0]:
                if self.proc.poll() is not None:
                    raise RunnerGone('exit %s' % self.proc.returncode)
                if time.time() - self.t0 > self.cfg.max_secs:
                    raise RunnerGone('wall limit')
                continue
            got = os.read(self.up, n - len(data))
            if not got:
                if self.proc.poll() is not None or self.down is not None:
                    raise RunnerGone('link closed')
                time.sleep(0.01)      # the game has not opened its end yet
                continue
            data += got
        return bytes(data)

    def _open_down(self):
        """The game opens up, then down, at its first pad read."""
        path = os.path.join(self.link, 'down')
        while True:
            try:
                fd = os.open(path, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:
                if self.proc.poll() is not None:
                    raise RunnerGone('exit %s' % self.proc.returncode)
                if time.time() - self.t0 > self.cfg.max_secs:
                    raise RunnerGone('wall limit')
                time.sleep(0.02)
        os.set_blocking(fd, True)
        self.down = fd

    def next_tick(self):
        """Wait for the next pad read; the game stands until `answer`."""
        if self.down is None:
            self._open_down()
        head = self._read(HEAD.size)
        _, self.tick, self.script_time, self.reads = HEAD.unpack(head)
        if time.time() - self.t0 > self.cfg.max_secs:
            raise RunnerGone('wall limit')
        return self.tick

    def mem(self, address, size):
        os.write(self.down, struct.pack('<cII', b'R', address, size))
        return self._read(size)

    def u32(self, address):
        return struct.unpack('<I', self.mem(address, 4))[0]

    def answer(self, p=NOTHING, skip=0):
        os.write(self.down, p.wire(skip))

    def command(self, text):
        """A ZX_CONTROL command (a screenshot); runs at the next vsync."""
        with open(os.path.join(self.dir, 'ctl', 'cmd'), 'a') as f:
            f.write(text + '\n')

    def pull_pad(self, at, frames=6):
        """Pad 1 pulled out at pad script time `at` (ZX_CONTROL padport) and put back `frames` vsyncs later, so a
        route made from the session pulls it at the same moment: the commands are kept in self.control
        (a session writes them to meta.json "control"; dm2corpus.py runs them before the runner's first vsync)."""
        # (put back by vsyncs: the pad script clock stands still while the game reads no pad)
        lines = ['until script>=%d 100000000' % at, 'padport 0', 'wait %d' % frames, 'padport 1']
        self.control = getattr(self, 'control', []) + lines
        for line in lines:
            self.command(line)

    def shot(self, name):
        path = os.path.join(self.dir, 'shots', name + '.png')
        self.command('shot ' + path)
        return path


class Recorder:
    """The answers as pad script items, by script time. Pad scripts OR buttons and hold a stick until the item ends,
    so an answer's runs of one button or one stick value become one item each."""

    def __init__(self):
        self.open = {}      # key -> [start, last]
        self.items = []
        self.last_time = None

    def add(self, script_time, p):
        now = {}
        for b in p.buttons:
            now[b] = None
        for name, v in (('RX', p.rx), ('RY', p.ry), ('LX', p.lx), ('LY', p.ly)):
            if v is not None:
                now['%s=%g' % (name, stick_value(stick_byte(v)))] = None
        for key in list(self.open):
            if key not in now:
                self._close(key, script_time)
        for key in now:
            if key not in self.open:
                self.open[key] = script_time

    def _close(self, key, end):
        start = self.open.pop(key)
        if end > start:
            self.items.append((start, '%dv:%s:%dv' % (start, key, end - start)))

    def finish(self, end):
        for key in list(self.open):
            self._close(key, end)
        self.items.sort()

    def text(self):
        return ','.join(i[1] for i in self.items)


class Stats:
    """Named numbers for a report: count(), add(), best(). `row` prints them in a fixed order."""

    def __init__(self):
        self.v = {}

    def count(self, name, n=1):
        self.v[name] = self.v.get(name, 0) + n

    def add(self, name, x):
        self.v[name] = self.v.get(name, 0.0) + x

    def best(self, name, x):
        if x > self.v.get(name, float('-inf')):
            self.v[name] = x

    def get(self, name, default=0):
        return self.v.get(name, default)

    def __getitem__(self, name):
        return self.v.get(name, 0)

    def row(self, names):
        return ' | '.join(fmt(self.v.get(n, 0)) for n in names)

    def json(self):
        return dict(sorted(self.v.items()))


def fmt(x):
    if isinstance(x, float):
        return '%.0f' % x if abs(x) >= 100 else '%.1f' % x
    return str(x)


class Trace:
    """A per-tick digest of what the game showed, written to a file: two runs compare tick for tick."""

    def __init__(self, path, every=1):
        self.f = open(path, 'w')
        self.h = hashlib.blake2b(digest_size=8)
        self.every = every
        self.n = 0

    def add(self, script_time, words):
        self.h.update(words)
        self.n += 1
        if self.n % self.every == 0:
            self.f.write('%d %s\n' % (script_time, self.h.hexdigest()))

    def close(self):
        self.f.write('end %d %s\n' % (self.n, self.h.hexdigest()))
        self.f.close()


def compare_traces(a, b):
    """The first line two traces differ on, or None."""
    with open(a) as fa, open(b) as fb:
        n = 0
        for la, lb in zip(fa, fb):
            n += 1
            if la != lb:
                return n, la.strip(), lb.strip()
    return None


def play(run, read, policy, max_ticks, on_tick=None, recorder=None, trace=None):
    """The loop. read(run) -> state, policy(state) -> a Pad or None (stop). max_ticks: pad reads answered."""
    last = None
    state = None
    n = 0
    try:
        while n < max_ticks:
            run.next_tick()
            n += 1
            state = read(run)
            if run.script_time == last:     # a second read in one vsync: the same answer
                run.answer(p if n > 1 else NOTHING)
                continue
            last = run.script_time
            p = policy(state)
            if p is None:
                run.answer(NOTHING)
                break
            if recorder:
                recorder.add(run.script_time, p)
            if trace and getattr(state, 'digest', None):
                trace.add(run.script_time, state.digest)
            if on_tick:
                on_tick(state, p)
            run.answer(p)
    except RunnerGone as e:
        print('runner gone:', e, 'at tick', run.tick, file=sys.stderr)
    if recorder:
        recorder.finish(run.script_time + 1)
    return n


# ---- geometry ------------------------------------------------------------------------------------

def wrap(a):
    """An angle into -pi .. pi."""
    return (a + math.pi) % (2 * math.pi) - math.pi


def clamp(x, low, high):
    return max(low, min(high, x))


def dist2(a, b):
    return (a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2


def save_json(path, data):
    with open(path, 'w') as f:
        json.dump(data, f, indent=1, sort_keys=True)
