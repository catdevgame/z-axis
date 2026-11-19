#!/usr/bin/env python3
"""Games booted one after another in one process, each against a process of its own (collmenu1 lane 3).

The collection's menu boots a game, the game exits to the menu, the menu boots the next. This check runs
that with no menu drawn: `ps2EntryRunner --sequence ai:STEP,dm2:STEP` (runtime/ps2_boot_sequence.h) boots
each game with its check's input, dumps and state log, stops it once its last checkpoint is dumped, tears
it down as an exit to the menu does and boots the next. Each game is then run by the same runner in a
fresh process (a one-step sequence: the same code path with no game before it), and the two compared:

  - the checker's values at every checkpoint, exactly: logiccheck's state() for AI (21 checkpoints),
    dm2logic's for DM2 (6), bmxlogic's for BMX XXX (17); and each side's checker verdict against the
    PCSX2 reference (the fresh side is expected to pass as the checker does);
  - every RAM word of every checkpoint dump. Words that differ between two fresh runs (noise) and words
    below their thread's sp in both dumps (dead stack, career_ab.py's rule) are counted, not listed.
    The rest are listed by region and by the nearest named data at or below them
    (readable_symbols.tsv; another game's through its bind table, $GAME_BIND_DIR/<game>.tsv).

The checkers are imported for their inputs and values (tooling/ai/tools/logiccheck.py,
tooling/dm2/tools/dm2logic.py, bmxlogic.py), never changed.

  python3 -I tooling/checks/seqcheck.py run ai,dm2 [dm2,ai ...] [--runner PATH] [--out DIR] [--keep]
                                           [--no-noise] [--env NAME=VALUE ...] [--list N]
  python3 -I tooling/checks/seqcheck.py step GAME PREFIX       # print a step file

Each sequence prints MATCH or DIFF per step; a DIFF names the first differing checkpoint and counts the
differing words. Exit 0 only when every step of every sequence matches. Everything is written under
--out (default $ZAXIS_SCRATCH/seqcheck/<time>): runner logs, step files, <sequence>/<step>-words.txt
(every listed word). Dumps are deleted once compared (--keep keeps them).

Environment: RUNNER (the runner, else --runner). Every runner gets a scratch HOME, null GS, the CPU GS
backend, SDL's dummy drivers, unpaced; no PS2X_* of the caller's environment reaches it (it would win
over every step's own values) except what --env gives (process-wide switches: PS2X_READABLE,
PS2X_EE_SCALE, ...).
"""
import array
import bisect
import collections
import contextlib
import importlib.util
import io
import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402

TOOLS = ztree.TOOLS    # (the tree's: readable_symbols.tsv)
RAM_SIZE = 32 << 20
GAME_ELF = {'ai': 'SLUS_203.27', 'dm2': 'SLUS_201.59', 'bmx': 'SLUS_204.15'}
# career_ab.py's rough maps.
REGIONS = {
    'ai': [(0x00000000, 0x00100000, 'below the ELF'), (0x00100000, 0x00279158, 'ELF text and data'),
           (0x00279158, 0x002B3B70, 'bss'), (0x002B3B70, 0x01EF0000, 'heap'), (0x01EF0000, 0x01F00000, 'main stack'),
           (0x01F00000, 0x02000000, 'above the main stack')],
    'dm2': [(0x00000000, 0x00100000, 'below the ELF'), (0x00100000, 0x0025B780, 'ELF text and data'),
            (0x0025B780, 0x003040B0, 'bss'), (0x003040B0, 0x01EF0000, 'heap'), (0x01EF0000, 0x01F00000, 'main stack'),
            (0x01F00000, 0x02000000, 'above the main stack')],
    'bmx': [(0x00000000, 0x00100000, 'below the ELF'), (0x00100000, 0x002A2680, 'ELF text and data'),
            (0x002A2680, 0x00314B30, 'bss'), (0x00314B30, 0x01EF0000, 'heap'), (0x01EF0000, 0x01F00000, 'main stack'),
            (0x01F00000, 0x02000000, 'above the main stack')],
}
STALL_SECONDS = 60   # a step with under 60 VBlanks in this long has hung (DM2 on BMX XXX's disc: one VBlank in 9 s)
NAMED_REACH = 0x1000  # a word further than this above the nearest named data is listed by region only (tables run to 0x800 and more: AI's voices)


def load_module(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def quiet(fn, *args):
    """fn's return value and what it printed."""
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        value = fn(*args)
    return value, out.getvalue()


# ---- the games: each checker's input, dumps, values and verdict ---------------------------------------------

class Game:
    """One game's check, from its checker module: what a step is given and what is compared."""

    def __init__(self, gid):
        self.id = gid
        ai = zaxis_paths.get('AI_WORKSPACE')
        dm2 = zaxis_paths.get('DM2_WORKSPACE')
        if gid == 'ai':
            self.mod = load_module('logiccheck', os.path.join(ai, 'tools', 'logiccheck.py'))
            m = self.mod
            self.elf = os.path.join(m.ROOT, 'recomp', 'disc-root', GAME_ELF[gid])
            self.cwd = None   # (run.sh runs it in recomp/run; nothing there is read, and nothing is written there)
            self.card_seed = os.path.join(m.ROOT, 'recomp', 'run', 'mc0')
            self.labels = [ck for ck, _ in m.checkpoints()]
            self.script = m.native_script()
            self.dumps = m.dump_spec()
            self.last_script = max(int(x.split('>=')[1]) for x in self.dumps.split(','))
            self.secs = 900
            # logiccheck.run's switches (run.sh: PS2X_DEBUG_UI=0, the CPU renderer; lc-when-free: unpaced).
            self.config = {'PS2X_SAVES': 'card',
                           'PS2X_STATE_LOG': ','.join('0x%X' % a for a in (m.MODE, m.PANEL, m.SECTION, m.GAUGE, m.FRAME,
                                                                           m.POS, m.POS + 4, m.POS + 8))}
        elif gid == 'dm2':
            self.mod = load_module('dm2logic', os.path.join(dm2, 'tools', 'dm2logic.py'))
            m = self.mod
            self.elf = os.environ.get('ELF_DM2') or os.path.join(m.ROOT, 'recomp', 'disc-root', GAME_ELF[gid])
            self.cwd = None
            self.card_seed = None
            self.labels = [ck for ck, _, _, _ in m.ref_values()]
            self.script = m.native_script()
            self.dumps = m.dump_spec()
            self.last_script = max(t for _, _, t, _ in m.ref_values())
            self.secs = 600
            self.config = {'PS2X_STATE_LOG': ','.join('0x%X' % a for a in (m.MODE, m.LEVEL_CURSOR, m.SESSION,
                                                                           m.LEVEL_FRAME, m.RIDER_TABLE))}
        elif gid == 'bmx':
            self.mod = load_module('bmxlogic', os.path.join(dm2, 'tools', 'bmxlogic.py'))
            m = self.mod
            self.elf = os.environ.get('ELF_BMX') or m.ELF
            self.cwd = None
            self.card_seed = None
            self.labels = [ck for ck, _, _, _, _ in m.ref_values()]
            self.script = m.native_script()
            self.dumps = m.dump_spec()
            self.last_script = max(t for _, _, t, _, _ in m.ref_values())
            self.secs = 900
            self.config = {'PS2X_MC_FD_BASE0': '1', 'PS2X_HOST_INPUT': '0', 'PS2X_PAD_STICK_CENTER': '0x7f',
                           'PS2X_STATE_LOG': ','.join('0x%X' % a for a in (m.MODE, m.LEVEL_FRAME, m.RIDER_TABLE))}
        else:
            raise SystemExit('unknown game %r (ai, dm2, bmx)' % gid)
        # The tick limit: the last checkpoint's script time, with room for loading and standing anchors.
        self.ticks = self.last_script * 2 + 6000

    def values(self, ram):
        if self.id == 'ai':
            return self.mod.state(ram, True)
        return self.mod.state(ram)

    def verdict(self, dump_dir, name):
        """(matches, of, first-divergence line) of the checker's own compare of these dumps."""
        m = self.mod
        saved = m.NATIVE_RAM
        # bmxlogic's and dm2logic's compare delete the dumps of a pass (dm2logic's since proof1): the steps' compare
        # below reads them after, and remove_dumps clears them.
        keys = ('BMXLOGIC_KEEP', 'DM2LOGIC_KEEP')
        kept = {k: os.environ.get(k) for k in keys}
        for k in keys:
            os.environ[k] = '1'
        m.NATIVE_RAM = dump_dir
        try:
            _, text = quiet(m.compare, name)
        finally:
            m.NATIVE_RAM = saved
            for k, v in kept.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
        # Checkpoint rows only (bmxlogic's last line, "17/17 MATCH; ...", is a summary).
        rows = [l for l in text.splitlines() if re.match(r'^[^\s/]+\s+(MATCH|DIFF|missing)\b', l)]
        matches = sum(1 for l in rows if re.match(r'^[^\s/]+\s+MATCH\b', l))
        first = next((l for l in text.splitlines() if 'first divergence' in l), '')
        return matches, len(self.labels), first.strip()

    def step_file(self, prefix, card, work):
        """A step's file: the checker's switches, dumps to prefix-<label>.ram."""
        os.makedirs(os.path.dirname(prefix), exist_ok=True)
        script_path = prefix + '-script.txt'
        with open(script_path, 'w') as f:
            f.write(self.script)
        lines = ['# seqcheck step: %s' % self.id,
                 'elf=%s' % self.elf,
                 'ticks=%d' % self.ticks,
                 'seconds=%d' % self.secs,
                 'stall_seconds=%d' % STALL_SECONDS,
                 'stop_after=%s-%s.sp' % (prefix, self.labels[-1]),
                 'mc_root=%s' % card,
                 'cwd=%s' % (self.cwd or work),
                 'PS2X_PAD_SCRIPT_CLOCK=vsync',
                 'PS2X_PAD_SCRIPT=@%s' % script_path,
                 'PS2X_DUMP_WHEN=%s' % self.dumps,
                 'PS2X_DUMP_PREFIX=%s' % prefix,
                 'PS2X_STATE_LOG_FILE=%s-state.log' % prefix]
        lines += ['%s=%s' % kv for kv in sorted(self.config.items())]
        path = prefix + '.step'
        with open(path, 'w') as f:
            f.write('\n'.join(lines) + '\n')
        return path

    def dump(self, prefix, label):
        return '%s-%s.ram' % (prefix, label)


# ---- symbols ------------------------------------------------------------------------------------------------

class Symbols:
    """The nearest named data or function at or below an address, per game."""

    def __init__(self, gid):
        names = {}
        table = os.path.join(TOOLS, 'readable_symbols.tsv')
        ai_data = {}
        for line in open(table):
            if line.startswith('#'):
                continue
            f = line.rstrip('\n').split('\t')
            if len(f) < 5 or f[0] not in ('data', 'function'):
                continue
            kind, name, address, _, game = f[:5]
            address = int(address, 16)
            if game == '' and kind == 'data':
                ai_data[address] = name
            if game == ('' if gid == 'ai' else gid):
                names.setdefault(address, (name, kind))
        if gid != 'ai':
            bind = os.path.join(zaxis_paths.get('GAME_BIND_DIR', required=False) or '', gid + '.tsv')
            if os.path.exists(bind):
                for line in open(bind):
                    if line.startswith('#'):
                        continue
                    f = line.rstrip('\n').split('\t')
                    if len(f) >= 4 and f[0] == 'data' and f[1] and f[3]:
                        name = ai_data.get(int(f[1], 16))
                        if name:
                            names.setdefault(int(f[3], 16), (name, 'data'))
        self.addresses = sorted(names)
        self.names = [names[a] for a in self.addresses]

    def name(self, address):
        i = bisect.bisect_right(self.addresses, address) - 1
        if i < 0 or address - self.addresses[i] > NAMED_REACH:
            return None
        name, kind = self.names[i]
        offset = address - self.addresses[i]
        return name + ('+0x%X' % offset if offset else '') + (' (code)' if kind == 'function' else '')

    def owner(self, address):
        """The named data a word belongs to (for grouping), None past NAMED_REACH."""
        i = bisect.bisect_right(self.addresses, address) - 1
        if i < 0 or address - self.addresses[i] > NAMED_REACH:
            return None
        return self.names[i][0]


def region_of(gid, address):
    for start, end, name in REGIONS[gid]:
        if start <= address < end:
            return name
    return 'above 32 MB'


# ---- RAM ----------------------------------------------------------------------------------------------------

def differing_words(path_a, path_b):
    """[(address, a, b)] of every word that differs; None when a dump is missing or cut short."""
    if not (os.path.exists(path_a) and os.path.exists(path_b)):
        return None
    a = open(path_a, 'rb').read()
    b = open(path_b, 'rb').read()
    if len(a) != len(b):
        return None
    out = []
    block = 1 << 14
    for at in range(0, len(a), block):
        if a[at:at + block] == b[at:at + block]:
            continue
        wa = array.array('I', a[at:at + block])
        wb = array.array('I', b[at:at + block])
        for i in range(len(wa)):
            if wa[i] != wb[i]:
                out.append((at + i * 4, wa[i], wb[i]))
    return out


def thread_stacks(ram_path):
    """[(stack base, size, sp)] from the dump's .sp file (StateProbe::writeThreadStacks)."""
    path = ram_path[:-4] + '.sp'
    if not os.path.exists(path):
        return None
    out = []
    for line in open(path):
        p = line.split()
        if len(p) >= 5:
            stack, size, sp = (int(x, 0) for x in p[2:5])
            # Either the stack's low end or its top (career_ab.py's Thread does the same).
            if stack - size <= sp < stack:
                stack -= size
            out.append((stack, size, sp))
    return out


def dead_stack(address, sides):
    def dead(threads):
        return threads is not None and any(s and s <= address < s + n and address < sp for s, n, sp in threads)
    return all(dead(t) for t in sides)


# ---- running ------------------------------------------------------------------------------------------------

def runner_env(args, home):
    env = {k: v for k, v in os.environ.items() if not k.startswith('PS2X_')}
    env.update({'HOME': home, 'PS2X_APP_SUPPORT_DIR': os.path.join(home, 'support'),
                'PS2X_APP_LOG_DIR': os.path.join(home, 'logs'), 'PS2X_GS_NULL': '1', 'PS2X_GS_BACKEND': 'cpu',
                'PS2X_UNPACED': '1', 'PS2X_DEBUG_UI': '0', 'PS2X_HEADLESS': '1',
                'SDL_VIDEODRIVER': 'dummy', 'SDL_AUDIODRIVER': 'dummy'})
    for item in args.env:
        k, _, v = item.partition('=')
        env[k] = v
    return env


def free_percent():
    try:
        text = subprocess.run(['memory_pressure', '-Q'], capture_output=True, text=True, timeout=20).stdout
        return int(re.search(r'(\d+)%', text).group(1))
    except (OSError, subprocess.SubprocessError, AttributeError):
        return 100


class Run:
    """One runner process: a sequence of steps (a fresh reference is a sequence of one)."""

    def __init__(self, args, games, order, folder):
        self.order = order
        self.folder = folder
        shutil.rmtree(folder, ignore_errors=True)
        os.makedirs(folder)
        self.home = os.path.join(folder, 'home')
        os.makedirs(os.path.join(self.home, 'support'))
        self.prefixes, items = [], []
        for i, gid in enumerate(order):
            prefix = os.path.join(folder, 'dumps', '%d-%s' % (i + 1, gid))
            card = os.path.join(folder, 'cards', '%d-%s' % (i + 1, gid), 'mc0')
            game = games[gid]
            if game.card_seed and os.path.isdir(game.card_seed):
                shutil.copytree(game.card_seed, card)
            else:
                os.makedirs(card)
            work = os.path.join(folder, 'work', '%d-%s' % (i + 1, gid))
            os.makedirs(work)
            items.append('%s:%s' % (gid, game.step_file(prefix, card, work)))
            self.prefixes.append(prefix)
        self.limit = sum(games[g].secs for g in order) + 60
        self.log = os.path.join(folder, 'runner.log')
        with open(self.log, 'w') as f:
            self.proc = subprocess.Popen([args.runner, '--sequence', ','.join(items)], stdout=f, stderr=subprocess.STDOUT,
                                         stdin=subprocess.DEVNULL, env=runner_env(args, self.home), cwd=folder,
                                         start_new_session=True)
        self.t0 = time.time()
        print('  started %s (pid %d): %s' % (os.path.basename(folder), self.proc.pid, ','.join(order)), flush=True)

    def poll(self):
        if self.proc.poll() is not None:
            return True
        if time.time() - self.t0 > self.limit:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait()
            print('  %s: killed after %d s' % (os.path.basename(self.folder), self.limit), flush=True)
            return True
        return False

    def step_logs(self):
        """[(end line, [lines])] per step, from the [seq] markers."""
        out, cur = [], None
        for line in open(self.log, errors='replace'):
            line = line.rstrip('\n')
            # A marker can land inside another thread's line: split it out.
            m = re.search(r'\[seq\] step \d+ of \d+: \S+ (begin|end: (.*))', line)
            if m:
                if cur is not None and line[:m.start()]:
                    cur.append(line[:m.start()])
                if m.group(1) == 'begin':
                    cur = []
                elif cur is not None:
                    out.append((m.group(2), cur))
                    cur = None
                continue
            if cur is not None:
                cur.append(line)
        if cur is not None:
            out.append(('no end (the process ended in the step)', cur))
        return out


def wait_all(runs):
    while True:
        if all(r.poll() for r in runs):
            return
        time.sleep(2)


def run_limited(args, jobs, start):
    """Starts jobs (callables returning a Run) at most args.jobs at a time, waiting for memory."""
    runs, live = [], []
    for job in jobs:
        while len(live) >= args.jobs or (live and free_percent() < 30):
            live = [r for r in live if not r.poll()]
            time.sleep(2)
        while free_percent() < 30:
            print('  waiting: under 30% memory free', flush=True)
            time.sleep(30)
        r = job()
        runs.append(r)
        live.append(r)
    wait_all(runs)
    return runs


# ---- comparing ----------------------------------------------------------------------------------------------

NUMBERS = re.compile(r'0x[0-9a-fA-F]+|\b[0-9a-fA-F]{6,}\b|\d+(\.\d+)?')


INSTALL_FACTS = (
    ('readable functions on', re.compile(r'\[readable\] (?:\w+: )?(\d+ of \d+) readable functions on')),
    ('override hooks called through the table', re.compile(r'\[readable\] \d+ on functions hooked by overrides, called through the table: ([^\[]*)')),
    ('matching overrides', re.compile(r'\[game_overrides\] applied (\d+) matching override')),
    ('disc files from', re.compile(r'\[disc\] files from (\S+)')),
    ('AI native draw', re.compile(r'\[native\] render path (\S+)')),
    ('DM2 native draw', re.compile(r'\[dm2native\] level meshes (native on the GPU renderer|through VU1)')),
)


def install_facts(lines):
    """{fact: value} of what a boot installed, from its log (sets for lists)."""
    out = {}
    for line in lines:
        for key, pattern in INSTALL_FACTS:
            m = pattern.search(line)
            if m:
                value = m.group(1).strip()
                if key.startswith('override hooks'):
                    out[key] = set(x.strip() for x in value.split(',') if x.strip())
                elif key == 'disc files from':
                    out.setdefault(key, set()).add(value)
                else:
                    out[key] = value
    return out


def log_kinds(lines, folder):
    """The step's log lines with numbers as # and its run's folder as <run>."""
    # Whole tagged lines only: another thread's text spliced into a line makes it one of a kind, and the
    # GS and DMA traces print their first calls only (a count per process, not per boot).
    return collections.Counter(NUMBERS.sub('#', l.replace(folder, '<run>')) for l in lines
                               if l.startswith('[') and l.count('[') == 1 and
                               not l.startswith(('[state]', '[dump]', '[pad] sync', '[gs:', '[sceDmaSend', '[frame:')))


def compare_step(args, game, symbols, seq_prefix, ref_prefix, noise, out_words, report):
    """Prints the step's result; True when it matches."""
    first_values = first_words = None
    totals = collections.Counter()
    by_region = collections.Counter()
    by_name = collections.Counter()
    name_labels = collections.defaultdict(set)
    region_labels = collections.defaultdict(set)
    rows = []
    sink = open(out_words, 'w')
    sink.write('# label address seq ref region name\n')
    for label in game.labels:
        s_path, r_path = game.dump(seq_prefix, label), game.dump(ref_prefix, label)
        if not os.path.exists(s_path):
            rows.append('    %-24s missing in the sequence' % label)
            first_values = first_values or label
            continue
        if not os.path.exists(r_path):
            rows.append('    %-24s missing in the fresh run' % label)
            first_values = first_values or label
            continue
        s_ram, r_ram = open(s_path, 'rb').read(), open(r_path, 'rb').read()
        sv, rv = game.values(s_ram), game.values(r_ram)
        del s_ram, r_ram
        vdiff = [k for k in sorted(set(sv) | set(rv), key=str) if repr(sv.get(k)) != repr(rv.get(k))]
        words = differing_words(s_path, r_path) or []
        sides = (thread_stacks(s_path), thread_stacks(r_path))
        mask = noise.get(label, set())
        kept, n_dead, n_noise = [], 0, 0
        for address, a, b in words:
            if address in mask:
                n_noise += 1
            elif dead_stack(address, sides):
                n_dead += 1
            else:
                kept.append((address, a, b))
        totals['words'] += len(kept)
        totals['dead'] += n_dead
        totals['noise'] += n_noise
        for address, a, b in kept:
            region = region_of(game.id, address)
            owner = symbols.owner(address) if region in ('ELF text and data', 'bss') else None
            key = owner or ('%s 0x%X0000' % (region, address >> 16) if region == 'heap' else region)
            by_region[region] += 1
            region_labels[region].add(label)
            by_name[key] += 1
            name_labels[key].add(label)
            sink.write('%s 0x%08X %08X %08X %s %s\n' % (label, address, a, b, region.replace(' ', '_'),
                                                       symbols.name(address) or '-'))
        if vdiff and first_values is None:
            first_values = label
        if kept and first_words is None:
            first_words = label
        rows.append('    %-24s %-5s values %s; RAM %s words differ (%d noise, %d dead stack)' % (
            label, 'MATCH' if not vdiff and not kept else 'DIFF',
            'equal' if not vdiff else 'differ: ' + ', '.join('%s %s/%s' % (k, short(sv.get(k)), short(rv.get(k))) for k in vdiff[:4]),
            format(len(kept), ','), n_noise, n_dead))
    sink.close()
    ok = first_values is None and first_words is None
    for r in rows:
        report(r)
    if not ok:
        report('    first differing checkpoint: values %s, RAM %s' % (first_values or 'none', first_words or 'none'))
        report('    by region (words, checkpoints): ' + ', '.join(
            '%s %s (%d)' % (r, format(n, ','), len(region_labels[r])) for r, n in by_region.most_common()))
        report('    by name (words summed over checkpoints, checkpoints):')
        for key, n in by_name.most_common(args.list):
            report('      %-48s %8s  %2d' % (key, format(n, ','), len(name_labels[key])))
        if len(by_name) > args.list:
            report('      ... %d more names or pages in %s' % (len(by_name) - args.list, out_words))
    else:
        os.remove(out_words)
    return ok, totals


def short(v):
    if isinstance(v, (tuple, list)):
        return '(' + ' '.join('%.3f' % x if isinstance(x, float) else str(x) for x in v[:3]) + ')'
    if isinstance(v, float):
        return '%.3f' % v
    if isinstance(v, int) and v > 9:
        return '%x' % v
    return str(v)


def noise_mask(game, prefix_a, prefix_b):
    """{label: {address}} of the words that differ between two fresh runs."""
    mask = {}
    for label in game.labels:
        w = differing_words(game.dump(prefix_a, label), game.dump(prefix_b, label))
        mask[label] = set(a for a, _, _ in w) if w is not None else set()
    return mask


def remove_dumps(prefix, labels):
    for label in labels:
        for ext in ('.ram', '.sp'):
            p = '%s-%s%s' % (prefix, label, ext)
            if os.path.exists(p):
                os.remove(p)


def cmd_run(args):
    if not args.runner or not os.access(args.runner, os.X_OK):
        raise SystemExit('no runner: --runner PATH or RUNNER')
    sequences = [s.split(',') for s in args.sequences]
    gids = sorted({g for s in sequences for g in s}, key=list(GAME_ELF).index)
    games = {g: Game(g) for g in gids}
    out = args.out or os.path.join(zaxis_paths.get('ZAXIS_SCRATCH'), 'seqcheck', time.strftime('%m%d-%H%M%S'))
    os.makedirs(out, exist_ok=True)
    summary = open(os.path.join(out, 'summary.txt'), 'w')

    def report(text):
        print(text, flush=True)
        summary.write(text + '\n')

    report('seqcheck: runner %s' % args.runner)
    report('out %s' % out)

    # The fresh references: each game alone, twice (the second only to find the words two fresh runs
    # differ in), two runners at a time.
    report('fresh runs: %s' % ', '.join(gids))
    jobs = []
    for g in gids:
        for k in (('a', 'b') if args.noise else ('a',)):
            jobs.append(lambda g=g, k=k: Run(args, games, [g], os.path.join(out, 'fresh-%s-%s' % (g, k))))
    fresh = {(os.path.basename(r.folder)): r for r in run_limited(args, jobs, time.time())}
    refs, noise = {}, {}
    for g in gids:
        ra = fresh['fresh-%s-a' % g]
        refs[g] = ra.prefixes[0]
        ends = ra.step_logs()
        report('  %s fresh: %s' % (g, ends[0][0] if ends else 'no step ran (see %s)' % ra.log))
        m, n, first = games[g].verdict(os.path.dirname(refs[g]), os.path.basename(refs[g]))
        report('  %s fresh against the PCSX2 reference: %d/%d MATCH; %s' % (g, m, n, first))
        if args.noise:
            rb = fresh['fresh-%s-b' % g]
            noise[g] = noise_mask(games[g], refs[g], rb.prefixes[0])
            counts = [len(noise[g][label]) for label in games[g].labels]
            report('  %s two fresh runs differ in %s words over %d checkpoints (most in one: %s)' % (
                g, format(sum(counts), ','), len(counts), format(max(counts) if counts else 0, ',')))
            if not args.keep:
                remove_dumps(rb.prefixes[0], games[g].labels)
        else:
            noise[g] = {}

    symbols = {g: Symbols(g) for g in gids}
    all_ok = True
    for order in sequences:
        name = 'seq-' + '-'.join(order)
        report('')
        report('sequence %s' % ','.join(order))
        r = run_limited(args, [lambda order=order, name=name: Run(args, games, order, os.path.join(out, name))], time.time())[0]
        ends = r.step_logs()
        for i, g in enumerate(order):
            end = ends[i][0] if i < len(ends) else 'did not run'
            report('  step %d %s: %s' % (i + 1, g, end))
            m, n, first = games[g].verdict(os.path.dirname(r.prefixes[i]), os.path.basename(r.prefixes[i]))
            report('    checker against the PCSX2 reference: %d/%d MATCH; %s' % (m, n, first))
            ok, totals = compare_step(args, games[g], symbols[g], r.prefixes[i], refs[g], noise[g],
                                      os.path.join(r.folder, '%d-%s-words.txt' % (i + 1, g)), report)
            if i < len(ends):
                ref_lines = fresh['fresh-%s-a' % g].step_logs()
                if ref_lines:
                    # What the boot installed: a hook a boot leaves out changes what runs even where the
                    # dumps cannot see it (rendering with the null GS).
                    s_facts, r_facts = install_facts(ends[i][1]), install_facts(ref_lines[0][1])
                    for key in sorted(set(s_facts) | set(r_facts)):
                        sv, rv = s_facts.get(key), r_facts.get(key)
                        if sv == rv:
                            continue
                        ok = False
                        if isinstance(sv, set) or isinstance(rv, set):
                            sv, rv = sv or set(), rv or set()
                            report('    %s: %d in the sequence, %d fresh; missing: %s; extra: %s' % (
                                key, len(sv), len(rv), ', '.join(sorted(rv - sv)) or 'none', ', '.join(sorted(sv - rv)) or 'none'))
                        else:
                            report('    %s: %s in the sequence, %s fresh' % (key, sv, rv))
                    a, b = log_kinds(ends[i][1], r.folder), log_kinds(ref_lines[0][1], fresh['fresh-%s-a' % g].folder)
                    only_seq = [k for k in a if k not in b]
                    only_ref = [k for k in b if k not in a]
                    if only_seq or only_ref:
                        report('    log lines (numbers as #) only in the sequence: %d, only fresh: %d' % (
                            len(only_seq), len(only_ref)))
                        for k in only_seq[:args.log_lines]:
                            report('      seq   %s' % k[:150])
                        for k in only_ref[:args.log_lines]:
                            report('      fresh %s' % k[:150])
            report('  step %d %s: %s%s' % (i + 1, g, 'MATCH' if ok else 'DIFF',
                                           '' if ok else ': %s words differ in all (%s noise, %s dead stack not counted)' % (
                                               format(totals['words'], ','), format(totals['noise'], ','),
                                               format(totals['dead'], ','))))
            all_ok = all_ok and ok
            if not args.keep:
                remove_dumps(r.prefixes[i], games[g].labels)
    if not args.keep:
        for g in gids:
            remove_dumps(refs[g], games[g].labels)
    report('')
    report('seqcheck: %s' % ('every step MATCH' if all_ok else 'DIFF'))
    summary.close()
    return 0 if all_ok else 1


def main():
    import argparse
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('run')
    r.add_argument('sequences', nargs='+', help='game,game[,...] (ai, dm2, bmx)')
    r.add_argument('--runner', default=os.environ.get('RUNNER'))
    r.add_argument('--out')
    r.add_argument('--keep', action='store_true', help='keep the RAM dumps')
    r.add_argument('--no-noise', dest='noise', action='store_false', help='one fresh run per game, no noise mask')
    r.add_argument('--env', action='append', default=[], help='NAME=VALUE for every runner (process-wide switches)')
    r.add_argument('--list', type=int, default=40, help='names or pages listed per step')
    r.add_argument('--log-lines', type=int, default=12)
    r.add_argument('--jobs', type=int, default=2, help='runners at once (at most 2)')
    s = sub.add_parser('step')
    s.add_argument('game')
    s.add_argument('prefix')
    args = p.parse_args()
    if args.cmd == 'step':
        g = Game(args.game)
        prefix = os.path.abspath(args.prefix)
        print(open(g.step_file(prefix, prefix + '-mc0', os.path.dirname(prefix))).read(), end='')
        return 0
    args.jobs = max(1, min(args.jobs, 2))
    return cmd_run(args)


if __name__ == '__main__':
    sys.exit(main())
