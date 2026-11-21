#!/usr/bin/env python3
"""The mechanical sweep (docs/READABLE.md, "Drafts and the sweep"): every function of a layer
that is not readable yet drafted by readable_draft.py, built, checked with
zx-fncheck against the recording corpus over every route, and registered as
Tested (identical on every recorded call of every route) or Draft, with the
results written to the work database's result files (MATCH3.md section 6).

    readable_sweep.py select  --run NAME --layer engine,sdk [--module M ...] [--limit N]
    readable_sweep.py draft   --run NAME [--only ADDR,...]
    readable_sweep.py build   --run NAME --build DIR [--runner]
    readable_sweep.py check   --run NAME --build DIR [--corpus DIR] [--jobs N] [--only ADDR,...]
    readable_sweep.py register --run NAME
    readable_sweep.py all     --run NAME --build DIR ...   (draft, build, check, register)
    readable_sweep.py status  --run NAME                   (stages, failure classes)
    readable_sweep.py classes --run NAME [--class C]       (the functions of each failure class)
    readable_sweep.py manifest-demote --only ADDR,...|@FILE --why CLASS   (no run state: the committed manifests)
    readable_sweep.py manifest-status                     (Verified, Tested, Draft, Planned per manifest; demotions by class)
    readable_sweep.py manifest-dead --triggers FILE [--only ADDR,...|@FILE --why script-dead] [--undo]   (dead-code flags)
    readable_sweep.py promotable --checked DIR[,DIR] [--out FILE] [--area engine,game,sce]
                                                          (the swept Tested that qualify for Verified, their shares)
    readable_sweep.py manifest-verify --only ADDR,...|@FILE [--drafts] [--undo [--why WHY]]   (Tested, with --drafts
                                                          Draft too, to Verified; or back; --drafts --undo --why: a Draft held)
    readable_sweep.py manifest-todos                      (the gate alone: open items in drafts, every Verified one listed)
    readable_sweep.py manifest-redraft [--only ADDR,...|@FILE] [--add [--why WHY]] [--build DIR [--target T ...]]
                                                          (no run state: the Planned rows drafted again, a list's functions added)

A run's state is `<work>/state.json` (default work: $ZAXIS_SCRATCH/sweep/<run>);
its functions are chosen once by `select` (from the work database: the layer's
functions with no readable registration, less those with no recompiled code of
their own (HLE stubs), those a game override hooks, and the ones listed with
--exclude), then each stage works on them all, or on --only.

Layout: batch files by module (the work database's module map), at most 40
functions a file, under --root (default by layer: engine engine/zge/sweep,
sdk ps2/sce/sweep, game games/aggressive/sweep), each
`<module>[_<n>].cpp` (functions with no module: by the map's blocks,
`blocks_<first address>.cpp`), in namespace `sweep::<module>`. The files say
they are generated (the marker line below); `draft` rewrites only such files.
A function that cannot be drafted, or whose draft does not compile, is
registered READABLE_PLANNED in its file instead, so the corpus records it.

Stages and their results (one JSON line each, workdb.py's format, in
<results>/<kind>.jsonl, default $ZAXIS_SCRATCH/workdb/results/<run>/):
  draft     drafted | failed (the reason; not lifted instructions are a failure)
  build     ok | compile_error (the first error; the function is set aside as Planned)
  check     per route: pass | fail | no_recording | crash | timeout; one
            zx-fncheck process per function (--cycles), every route's
            recording of it in the corpus (a memory fault in one route ends
            that process; the routes after it are checked in another)
  failure   the stage and the failure class (below)
  status    Tested | Draft | Planned, as registered
Failure classes: draft:<reason>, not_lifted:<op>, compile:<message with names
taken out>, and for checks the first difference: call_target, call_count,
call_extra, call_arg, call_arg_float, call_arg_qword, result_v0, result_f0,
ram, reads_uncovered, fill, callee_saved, vu0, io, cop0, cycles, no_return,
fault, original_replay (the recording does not replay: a recorder problem),
crash, timeout.

`manifest-demote` works on the committed manifests alone (after the run's state is gone,
or for a bisection over the merged manifests): a Tested function goes back to Draft with
`demoted: game:CLASS` in its reason column (logiccheck, boot, career, int_f0, ...);
`--promote` takes such a demotion back (with `--drafts`, also a Draft with no reason: one set
aside whole without its own failure, checked again since).

`promotable` (drafter19) lists the swept Tested functions that qualify for Verified (on by
default): over the given fncheck_corpus.py run folders (made with --cycles --safepoints
--markers), every recorded call identical with EE cycles (no call skipped, the original's replay
clean, no fault), no entry block some call reached that no file marked, no loop head branched back
to without ee::loopBack, the markers in order, no interruption at a safe point that differs, and
not in tools/checks/readable_promote_aside.txt (the 445 sweepfix set aside, functions a game check
pinned), not registered by hand as well (its row should have gone) or held back (`held: WHY` in the reason column, by `manifest-verify --undo --why`). It
prints the counts by area (engine zge/sweep, game aggressive/sweep, sce sce/sweep), why the rest do
not qualify, and their share of the live-play profile (recomp/traces/live-fnprof.csv, self time)
and of the code (Ghidra's function sizes). `manifest-verify` changes Tested to Verified for a
list (a batch, after its checks: logiccheck 21/21 with the defaults, the career A/B, dm2logic).

The game layer's agent runs it the same way with --layer game (its root is
games/aggressive/sweep); the options above are the interface.
"""
import argparse
import collections
import concurrent.futures
import datetime
import fcntl
import hashlib
import json
import os
import re
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
import readable_symbols
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ZAXIS_TREE = ztree.TREE
WORKSPACE = zaxis_paths.get('AI_WORKSPACE', required=False) or ''  # tools/, recomp/ (its data links)
GAME_SRC = ztree.TREE   # (its readable code: ztree.walk, key_of, readable)
import zaxis_paths  # noqa: E402  (the tree's readable folders: walk, key_of, readable)
sys.path.insert(0, HERE)

MARKER = '// readable_sweep.py: generated batch file'
DEFAULT_ROOTS = {'engine': 'zge/sweep', 'sdk': 'sce/sweep', 'game': 'aggressive/sweep'}
WORKDB_PY = zaxis_paths.get('WORKDB_PY', required=False)
CORPUS = os.path.join(SCRATCH_ROOT, 'reccorpus', 'v5')
PER_FILE = 40
# Modules whose functions reach hardware registers through pointers (a DMA
# channel's, the GS's): their drafts decide each access at run time
# (readable_draft.py --io-dynamic). A function that fails a check with an I/O
# difference is drafted that way too when it is drafted again.
IO_MODULES = {'sdk:libdma', 'sdk:libgraph', 'sdk:libdev', 'sdk:libipu', 'sdk:libkernl', 'sdk:libcdvd', 'sdk:libpad',
              'sdk:libmpeg', 'sdk:libsdr', 'sdk:libmc', 'sdk:libpc', 'sdk:libsk'}


def now():
    return datetime.datetime.now().isoformat(timespec='seconds')


def log(msg):
    print('%s %s' % (time.strftime('%H:%M:%S'), msg), flush=True)


# ---------------------------------------------------------------------------
# State, results
# ---------------------------------------------------------------------------

class Run:
    def __init__(self, args):
        self.args = args
        self.name = args.run
        self.work = os.path.abspath(os.path.expanduser(args.work or os.path.join(SCRATCH_ROOT, 'sweep', self.name)))
        self.results = os.path.abspath(os.path.expanduser(args.results or os.path.join(
            SCRATCH_ROOT, 'workdb', 'results', self.name)))
        os.makedirs(self.work, exist_ok=True)
        self.path = os.path.join(self.work, 'state.json')
        self.state = json.load(open(self.path)) if os.path.exists(self.path) else {'run': self.name, 'functions': {}}
        self.generator = generator_version()

    def save(self):
        tmp = self.path + '.tmp'
        json.dump(self.state, open(tmp, 'w'), indent=0)
        os.replace(tmp, self.path)

    @property
    def functions(self):
        return self.state['functions']

    def selected(self, only=None):
        keys = sorted(self.functions, key=lambda k: int(k, 16))
        if only:
            want = set('0x%08x' % int(x, 16) for x in only.split(',') if x.strip())
            keys = [k for k in keys if k in want]
        failed = getattr(self.args, 'failed', None)
        if failed:
            classes = [c.strip() for c in failed.split(',') if c.strip()]
            keys = [k for k in keys if self.functions[k].get('failure') and
                    any(self.functions[k]['failure']['class'].startswith(c) for c in classes)]
        return keys

    def emit(self, kind, addr, **fields):
        rec = {'kind': kind, 'addr': '0x%08X' % int(addr, 16) if isinstance(addr, str) else '0x%08X' % addr,
               'run': self.name, 'generator': self.generator, 'ts': now(), 'agent': self.args.agent}
        rec.update({k: v for k, v in fields.items() if v is not None})
        path = os.path.join(self.results, kind + '.jsonl')
        os.makedirs(self.results, exist_ok=True)
        with open(path, 'a') as f:
            fcntl.flock(f, fcntl.LOCK_EX)
            f.write(json.dumps(rec) + '\n')

    def failure(self, addr, stage, cls, detail=None):
        f = self.functions[addr]
        f['failure'] = {'stage': stage, 'class': cls, 'detail': (detail or '')[:300]}
        self.emit('failure', addr, stage=stage, failure_class=cls, detail=(detail or '')[:300])


def generator_version():
    """readable_draft.py's commit, or a hash of the file when it differs from it."""
    path = os.path.join(ztree.TOOLS, 'readable_draft.py')
    digest = hashlib.sha1(open(path, 'rb').read()).hexdigest()[:10]
    try:
        commit = subprocess.run(['git', '-C', ztree.TOOLS, 'log', '-1', '--format=%h', '--', 'readable_draft.py'],
                                capture_output=True, text=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', ztree.TOOLS, 'diff', '--quiet', 'HEAD', '--', 'readable_draft.py']).returncode
    except OSError:
        commit, dirty = '', 1
    return 'readable_draft.py %s' % (commit if commit and not dirty else 'file-' + digest)


def workdb_query(sql):
    p = subprocess.run([sys.executable, '-I', WORKDB_PY, 'query', sql, '--json'], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit('readable_sweep.py: workdb query failed: %s' % p.stderr.strip()[-400:])
    return json.loads(p.stdout)


def workspace():
    import readable_draft as rd
    generated = os.environ.get('ZX_RUNNER_GENERATED_DIR') or os.path.join(WORKSPACE, 'recomp', 'generated')
    ws = rd.Workspace(WORKSPACE, generated, os.path.join(zaxis_paths.get('DM2_WORKSPACE'), 'ai-decomp.c'))
    # A draft whose call path reaches an indirect call runs on a fiber
    # (READABLE_FUNCTION_WAITS): the call graph cannot see what such a call
    # reaches, and a loader that waits on a file read through a device's
    # function pointer stopped the game with Tested on (round 14).
    ws.indirect_waits = True
    return rd, ws


def ident(text):
    s = re.sub(r'^sdk:', '', text or '')
    s = re.sub(r'[^A-Za-z0-9_]', '_', s)
    return s if s and not s[0].isdigit() else 'm_' + s


# ---------------------------------------------------------------------------
# select
# ---------------------------------------------------------------------------

def cmd_select(run):
    args = run.args
    layers = [l.strip() for l in args.layer.split(',') if l.strip()]
    rows = workdb_query("SELECT addr, layer, coalesce(module, '') AS module, block, insns FROM ai_functions "
                        "WHERE status IS NULL AND layer IN (%s) ORDER BY addr" %
                        ', '.join("'%s'" % l for l in layers))
    rd, ws = workspace()
    exclude = set()
    for x in (args.exclude or '').split(','):
        if x.strip():
            exclude.add(int(x, 16))
    picked, skipped = [], collections.Counter()
    for r in rows:
        a = int(r['addr'])
        if args.module and r['module'] not in args.module:
            continue
        why = None
        if a in ws.readable:
            why = 'registered'
        elif a in exclude:
            why = 'excluded'
        elif a in ws.hooked:
            why = 'hooked by a game override'
        else:
            p = ws.parsed(a)
            if p is None:
                why = 'no generated file'
            elif not p.insns or p.start is None:
                why = 'no recompiled code (an HLE stub)'
        if why:
            skipped[why] += 1
            continue
        picked.append(r)
    if args.limit:
        picked = picked[:args.limit]
    # Files: by module, then by block for functions with none, at most PER_FILE a file,
    # under the layer's root.
    groups = collections.OrderedDict()
    for r in picked:
        key = ident(r['module']) if r['module'] else None
        groups.setdefault((r['layer'], key or None), []).append(r)
    files = {}
    for (layer, key), members in groups.items():
        root = args.root or DEFAULT_ROOTS.get(layer, 'sweep')
        members.sort(key=lambda r: int(r['addr']))
        if key is None:
            # No module: consecutive functions, a file per PER_FILE, named by the first address.
            for k in range(0, len(members), PER_FILE):
                chunk = members[k:k + PER_FILE]
                files[(root, 'blocks_%08x' % int(chunk[0]['addr']))] = chunk
        else:
            parts = [members[k:k + PER_FILE] for k in range(0, len(members), PER_FILE)]
            for n, chunk in enumerate(parts):
                files[(root, key if len(parts) == 1 else '%s_%d' % (key, n + 1))] = chunk
    roots = sorted(set(root for root, _ in files))
    for (root, stem), chunk in files.items():
        for r in chunk:
            k = '0x%08x' % int(r['addr'])
            old = run.functions.get(k, {})
            old.update({'layer': r['layer'], 'module': r['module'], 'block': r['block'], 'insns': r['insns'],
                        'file': os.path.join(root, stem + '.cpp'), 'namespace': 'sweep::' + stem})
            if r['module'] in IO_MODULES or (r['layer'] == 'sdk' and not r['module']):
                old['io_dynamic'] = True
            run.functions[k] = old
    run.state['root'] = ', '.join(roots)
    run.save()
    log('selected %d functions in %d files under %s (skipped: %s)' % (
        len(picked), len(files), ', '.join(roots), ', '.join('%s %d' % kv for kv in skipped.most_common())))


# ---------------------------------------------------------------------------
# draft
# ---------------------------------------------------------------------------

def not_lifted_class(d):
    ops = sorted(set(i.op for i in d.lifter.unsupported))
    return 'not_lifted:' + ','.join(ops[:3])


def manifest_path(root):
    return ztree.readable(GAME_SRC, root, 'manifest.txt')


def manifest_header(run, root):
    return ['# readable_sweep.py manifest (docs/READABLE.md, "Drafts and the sweep"): the swept functions of %s, generated at' % root,
            '# build time by game.cmake (readable_draft.py --manifest) into the build dir; the batch files are not',
            '# committed. Columns (tab-separated): %s.' % ', '.join(__import__('readable_draft').MANIFEST_COLUMNS),
            '# generator: %s' % run.generator]


def entry_of(run, k):
    f = run.functions[k]
    return {'address': int(k, 16), 'status': f.get('status', 'Planned'), 'file': f['file'], 'namespace': f['namespace'],
            'macro': f.get('macro') if f.get('status') in ('Draft', 'Tested', 'Verified') else '',
            'flags': {'io'} if f.get('io_dynamic') else set(), 'name': f.get('name') or '',
            'signature': f.get('signature') or '', 'elf': {int(a, 16): w for a, w in (f.get('elf') or {}).items()},
            'reason': f.get('reason') or ''}


def write_manifests(run):
    """The manifests of the run's roots, from its state (statuses as registered)."""
    import readable_draft as rd
    roots = collections.defaultdict(list)
    for k, f in run.functions.items():
        roots[os.path.dirname(f['file'])].append(entry_of(run, k))
    # The functions' data references (the ELF's words holding their
    # addresses), pinned for the build's void-v0 decision (rd.v0_unread).
    ws = getattr(run, '_refs_ws', None)
    if ws is None:
        ws = run._refs_ws = workspace()
    for root, entries in roots.items():
        rd.pin_data_refs(ws, entries)
        # The dead-code flags (manifest-dead) are the manifest's own: kept.
        path = manifest_path(root)
        if os.path.exists(path):
            old = {e['address']: e for e in rd.read_manifest(path)[1]}
            for e in entries:
                was = old.get(e['address'])
                e['flags'] = set(e['flags']) | (was['flags'] & KEPT_FLAGS if was else set())
                # data-dead's reason names the game whose data uses it (paths5)
                if was and 'data-dead' in was['flags'] and not e.get('reason'):
                    e['reason'] = was['reason']
        rd.write_manifest(path, manifest_header(run, root), entries)
    return sorted(roots)


def draft_file(run, rd, ws, path, keys, keep_planned):
    """Drafts one batch file's functions (as the build will) to learn each one's
    registration, name, Ghidra signature, ELF words and failures."""
    import argparse as ap
    ns = run.functions[keys[0]]['namespace']
    for k in keys:
        fail = run.functions[k].get('failure') or {}
        if fail.get('stage') == 'check' and (fail.get('class') in ('io', 'fill:io') or fail.get('class') == 'fault' and
                                             re.search(r'touched 0x0000[0-9a-f]{4}\b', fail.get('detail', ''))):
            run.functions[k]['io_dynamic'] = True
    args = ap.Namespace(name=None, status='Draft', no_ghidra=True, namespace=ns, workdb=None, stable=True,
                        io_dynamic={int(k, 16) for k in keys if run.functions[k].get('io_dynamic')})
    addrs = [int(k, 16) for k in keys if k not in keep_planned]
    ws.elf_log = {}
    ws.elf_by_fn = {}
    text, drafts, failures = rd.draft_batch(ws, addrs, args, skip_failures=True)
    ws.drafting = None
    bad = [d for d in drafts if d.lifter.unsupported]
    if bad:
        for d in bad:
            failures[d.address] = not_lifted_class(d)
    # The ELF words each function's draft read (its row pins them).
    elf = {'0x%08x' % a: words for a, words in ws.elf_by_fn.items()}
    ws.elf_log = ws.elf_by_fn = None
    for k, why in keep_planned.items():
        failures[int(k, 16)] = why
    out = {}
    for d in drafts:
        if d.address in failures:
            continue
        dec = ws.decomp(d.address)
        sig = None
        if dec:
            # As Draft.ghidra_signature reads it: the return type, name, parameters.
            m = re.search(r'^(.*?)\b(\w+)\s*\(([^)]*)\)\s*\n\s*\{', dec[1], re.S | re.M)
            if m:
                sig = '%s %s(%s)' % (m.group(1).strip().split('\n')[-1].strip(), m.group(2), ' '.join(m.group(3).split()))
        out['0x%08x' % d.address] = {'name': d.name, 'macro': d.macro, 'lines': d.span[1] - d.span[0] + 1,
                                     'gotos': len(d.goto_targets), 'todo': sorted(set(d.todo))[:6], 'signature': sig}
    return out, failures, elf


def cmd_draft(run):
    """Drafts the functions and writes the manifests (the build generates the
    batch files from them); commented copies for reading go to <work>/commented."""
    rd, ws = workspace()
    keys = run.selected(run.args.only)
    by_file = collections.OrderedDict()
    for k in sorted(run.functions, key=lambda k: int(k, 16)):
        by_file.setdefault(run.functions[k]['file'], []).append(k)
    files = sorted(set(run.functions[k]['file'] for k in keys))
    t0 = time.time()
    counts = collections.Counter()
    for n, path in enumerate(files):
        members = by_file[path]
        keep_planned = {k: run.functions[k]['failure']['class'] for k in members
                        if k not in keys and run.functions[k].get('status') == 'Planned' and run.functions[k].get('failure')}
        drafted, failures, elf = draft_file(run, rd, ws, path, members, keep_planned)
        for k in members:
            f = run.functions[k]
            f['elf'] = {'%X' % a: w for a, w in elf.get(k, {}).items()}
            if k in drafted:
                same = f.get('macro') == drafted[k]['macro'] and f.get('name') == drafted[k]['name'] and \
                    f.get('status') in ('Draft', 'Tested')
                f.update(drafted[k])
                f.pop('reason', None)
                if same and k not in keys:
                    continue
                if k in keys:
                    f['status'] = 'Draft' if f.get('status') != 'Tested' or not same else f['status']
                    f['stage'] = 'drafted'
                    if not same:
                        f.pop('failure', None)
                        f.pop('checks', None)
                    run.emit('draft', k, result='drafted', file=path, lines=drafted[k]['lines'],
                             gotos=drafted[k]['gotos'], name=drafted[k]['name'])
                    counts['drafted'] += 1
            else:
                why = failures.get(int(k, 16), 'not drafted')
                f['status'] = 'Planned'
                f['stage'] = 'draft_failed'
                f['reason'] = why[:200]
                f.pop('checks', None)
                if k in keys:
                    cls = why if why.startswith(('not_lifted:', 'compile:')) else 'draft:' + re.sub(
                        r'0x[0-9a-f]+', 'ADDR', why.split(':')[0] if why.startswith('generator error') else why)[:80]
                    run.emit('draft', k, result='failed', reason=why[:300], file=path)
                    run.failure(k, 'draft' if not cls.startswith('compile:') else 'build', cls, why)
                    counts['failed'] += 1
        run.save()  # a stopped draft keeps the files done so far
        if (n + 1) % 10 == 0:
            log('drafted %d of %d files (%.0f s)' % (n + 1, len(files), time.time() - t0))
    run.save()
    roots = write_manifests(run)
    log('draft: %d functions in %d files: %s; manifests %s (%.0f s)' % (
        len(keys), len(files), dict(counts), ', '.join(roots), time.time() - t0))
    if run.args.commented:
        commented(run, rd)


def commented(run, rd):
    """Copies of the batch files with Ghidra's decompilation and the work
    database's card in each draft, for reading (never the source tree)."""
    _, ws = workspace()
    out = os.path.join(run.work, 'commented')
    manifests = [manifest_path(r) for r in sorted(set(os.path.dirname(f['file']) for f in run.functions.values()))]
    n = rd.generate_from_manifests(ws, manifests, out, comments=True, workdb=None if run.args.no_workdb else WORKDB_PY)
    log('commented copies: %d files under %s' % (n, out))


# ---------------------------------------------------------------------------
# build
# ---------------------------------------------------------------------------

DIAG = re.compile(r'^([^:\n ][^:\n]*):(\d+):(\d+): (fatal error|error|note|warning): (.*)$', re.M)


def spans_of(build):
    path = os.path.join(build, 'runtime', 'readable_sweep', 'spans.json')
    return json.load(open(path)) if os.path.exists(path) else {}


def draft_errors(run, out, build):
    """{function key: first error message} for the errors the compiler output
    has, each mapped to the draft whose lines hold it or one of its notes (an
    error in a header names the draft's line in an "in instantiation" note)."""
    spans = collections.defaultdict(list)
    gen = os.path.join(build, 'runtime', 'readable_sweep')
    for path, items in spans_of(build).items():
        for a, (lo, hi) in items.items():
            spans[os.path.realpath(os.path.join(gen, path))].append((lo, hi, '0x%08x' % int(a, 16)))
    hit = {}
    current = None   # (message, mapped?)
    unmapped = []
    for path, line, col, kind, msg in DIAG.findall(out):
        if kind in ('error', 'fatal error'):
            if current and not current[1]:
                unmapped.append(current[0])
            current = [msg, False]
        elif kind == 'warning' or current is None:
            continue
        if current[1]:
            continue
        full = os.path.realpath(path if os.path.isabs(path) else os.path.join(build, path))
        for a, b, k in spans.get(full, ()):
            if a <= int(line) <= b and k in run.functions:
                hit.setdefault(k, current[0])
                current[1] = True
                break
    if current and not current[1]:
        unmapped.append(current[0])
    return hit, unmapped


def error_class(msg):
    m = re.sub(r"'[^']*'", 'X', msg)
    m = re.sub(r'"[^"]*"', 'X', m)
    m = re.sub(r'0x[0-9a-fA-F]+|\b\d+\b', 'N', m)
    return 'compile:' + m.strip()[:90]


def set_planned(run, key, why):
    """The function is registered READABLE_PLANNED (in its manifest) instead."""
    f = run.functions[key]
    f['status'] = 'Planned'
    f['stage'] = 'build_failed'
    f['reason'] = why[:200]
    f.pop('checks', None)


def cmake(build):
    p = subprocess.run(['cmake', build], capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit('readable_sweep.py: cmake failed:\n' + (p.stdout + p.stderr)[-3000:])


def cmd_build(run, targets=None):
    build = os.path.abspath(run.args.build)
    targets = targets or (['zx-fncheck', 'zx-runner'] if run.args.runner else ['zx-fncheck'])
    write_manifests(run)
    cmake(build)
    keys = set(run.selected(run.args.only))
    t0 = time.time()
    for attempt in range(40):
        p = subprocess.run(['ninja', '-C', build, '-k', '0', '-j', str(run.args.build_jobs)] + targets,
                           capture_output=True, text=True)
        out = p.stdout + p.stderr
        if p.returncode == 0:
            break
        hit, unmapped = draft_errors(run, out, build)
        if not hit:
            # An error in a batch file outside every draft (its constants): the
            # file's drafts are set aside, so the rest still build.
            gen = os.path.realpath(os.path.join(build, 'runtime', 'readable_sweep'))
            for path, line, col, kind, msg in DIAG.findall(out):
                full = os.path.realpath(path if os.path.isabs(path) else os.path.join(build, path))
                if kind in ('error', 'fatal error') and full.startswith(gen + os.sep):
                    rel = os.path.relpath(full, gen)
                    for k, f in run.functions.items():
                        if f.get('file') == rel and f.get('status') in ('Draft', 'Tested'):
                            hit.setdefault(k, 'its batch file does not compile: %s' % msg)
        if not hit:
            gen = os.path.realpath(os.path.join(build, 'runtime', 'readable_sweep'))
            files = {os.path.realpath(p if os.path.isabs(p) else os.path.join(build, p))
                     for p, _, _, kind, _ in DIAG.findall(out) if kind in ('error', 'fatal error')}
            mine = {os.path.realpath(os.path.join(gen, f['file'])) for f in run.functions.values()}
            if files and all(f.startswith(gen + os.sep) and f not in mine for f in files):
                # Another layer's batch files (another run's manifest): its run sets them aside.
                log('build: the errors left are in other runs\' batch files: %s' % ', '.join(
                    sorted(os.path.relpath(f, gen) for f in files)[:6]))
                run.save()
                write_manifests(run)
                sys.exit(3)
            sys.exit('readable_sweep.py: the build fails outside the drafts:\n' + out[-4000:])
        for k, msg in hit.items():
            set_planned(run, k, 'does not compile: ' + msg)
            run.emit('build', k, result='compile_error', error=msg[:300], build_dir=build)
            run.failure(k, 'build', error_class(msg), msg)
        run.save()
        write_manifests(run)
        log('build attempt %d: %d drafts did not compile, set aside as Planned' % (attempt + 1, len(hit)))
    else:
        sys.exit('readable_sweep.py: the build still fails after 40 attempts')
    spans = spans_of(build)
    built = {'0x%08x' % int(a, 16) for items in spans.values() for a in items}
    for k in keys:
        f = run.functions[k]
        f['span'] = k in built
        if f.get('status') in ('Draft', 'Tested') and f.get('stage') == 'drafted':
            run.emit('build', k, result='ok', build_dir=build)
            f['stage'] = 'built'
    run.save()
    log('build: ok (%.0f s)' % (time.time() - t0))


# ---------------------------------------------------------------------------
# check
# ---------------------------------------------------------------------------

RESULT = re.compile(r'^\[fncheck:result\] (.*)$', re.M)
FAULT = re.compile(r'^\[fncheck:fault\] (.*)$', re.M)
FIELD = re.compile(r'(\w+)=("(?:[^"]*)"|\S+)')


def fields(text):
    return {k: (v[1:-1] if v.startswith('"') else v) for k, v in FIELD.findall(text)}


def check_class(why):
    rules = [
        # zx-fncheck's checks of round `coverage`: f0 left by an integer
        # function, a callee's stack arguments
        (r'^(with another fill: )?f0 left = ', 'int_f0'), (r'stack argument byte at', 'stack_args'),
        # (drafter16) v0 left by a void function a caller reads it after
        (r'^(with another fill: )?v0 left = ', 'void_v0'),
        (r'I/O access', 'io'), (r'goes to .*, recorded', 'call_target'), (r'not in the recording', 'call_extra'),
        (r'made \d+ calls, recorded', 'call_count'), (r'quadword a\d', 'call_arg_qword'),
        (r': a\d+ = ', 'call_arg'), (r': f1\d', 'call_arg_float'), (r'I/O access', 'io'),
        (r'did not return', 'no_return'), (r'bytes read that the recording does not hold', 'reads_uncovered'),
        (r'COP0 status', 'cop0'), (r'^v0 = |\bv0 = ', 'result_v0'), (r'\bf0 = ', 'result_f0'), (r'VU0', 'vu0'),
        (r'not preserved', 'callee_saved'), (r'bytes of RAM differ', 'ram'), (r'charges \d+ EE cycles', 'cycles'),
        (r'^r\d+ = ', 'registers'), (r'hi/lo', 'registers'),
    ]
    fill = why.startswith('with another fill: ')
    for pat, cls in rules:
        if re.search(pat, why):
            return ('fill:' + cls) if fill else cls
    return 'other'


def corpus_files(corpus):
    """{address: [(route, path)]} of every recording in the corpus' route folders."""
    out = collections.defaultdict(list)
    for route in sorted(os.listdir(corpus)):
        folder = os.path.join(corpus, route)
        if not os.path.isdir(folder) or route == 'work':
            continue
        for name in os.listdir(folder):
            m = re.match(r'^([0-9a-f]{8})\.fnrec$', name)
            if m:
                out['0x' + m.group(1)].append((route, os.path.join(folder, name)))
    return out


def check_one(fncheck, key, files, timeout):
    """{route: result fields} for one function over its files, a process per run
    of files (a memory fault ends a process: the files after it get another)."""
    results = {}
    todo = list(files)
    while todo:
        cmd = [fncheck, '--cycles', '--show', '1', '--only', key] + [p for _, p in todo]
        status = None
        try:
            p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
            out, status = p.stdout + p.stderr, p.returncode
        except subprocess.TimeoutExpired as e:
            out = e.stdout.decode('latin-1') if isinstance(e.stdout, bytes) else (e.stdout or '')
        route_of = {path: r for r, path in todo}
        for m in RESULT.findall(out):
            f = fields(m)
            if f.get('file') in route_of:
                results[route_of[f['file']]] = f
        rest = [(r, path) for r, path in todo if r not in results]
        if rest:
            fault = FAULT.search(out)
            first = rest[0][0]
            if fault and fields(fault.group(1)).get('file') in route_of:
                f = fields(fault.group(1))
                before = re.search(r'\[fncheck\] before it: ([^\n]*)', out)
                results[route_of[f['file']]] = {'result': 'fault', 'detail': 'call %s (%s) touched %s%s' % (
                    f.get('call'), f.get('phase'), f.get('touched'), ('; before it: ' + before.group(1)) if before else '')}
            elif status is None:
                results[first] = {'result': 'timeout', 'detail': '%d s' % timeout}
            else:
                results[first] = {'result': 'crash', 'detail': 'exit %s: %s' % (status, out[-300:].replace('\n', ' '))}
        todo = [(r, path) for r, path in todo if r not in results]
    return results


def cmd_check(run):
    args = run.args
    fncheck = os.path.join(os.path.abspath(args.build), 'runtime', 'zx-fncheck')
    corpus = os.path.abspath(os.path.expanduser(args.corpus or CORPUS))
    rec = corpus_files(corpus)
    keys = [k for k in run.selected(args.only) if run.functions[k].get('status') in ('Draft', 'Tested')
            and run.functions[k].get('span')]
    log('check: %d functions over the corpus %s (%d jobs)' % (len(keys), corpus, args.jobs))
    t0 = time.time()
    counts = collections.Counter()
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        futures = {pool.submit(check_one, fncheck, k, rec.get(k, []), args.timeout): k for k in keys if rec.get(k)}
        for k in keys:
            if not rec.get(k):
                f = run.functions[k]
                f['checks'] = {}
                f['stage'] = 'checked'
                run.emit('check', k, route='*', result='no_recording')
                counts['no_recording'] += 1
        for n, fut in enumerate(concurrent.futures.as_completed(futures)):
            k = futures[fut]
            f = run.functions[k]
            try:
                res = fut.result()
            except Exception as e:  # noqa
                res = {'*': {'result': 'crash', 'detail': str(e)}}
            checks = {}
            first_fail = None
            for route, r in sorted(res.items()):
                if 'recorded' in r:
                    recorded = int(r['recorded'])
                    bad = int(r['readable_bad'])
                    orig_bad = int(r['original_bad'])
                    if recorded == 0:
                        result, cls, detail = 'no_recording', None, None
                    elif orig_bad:
                        result, cls, detail = 'fail', 'original_replay', r.get('first_original')
                    elif bad:
                        result, cls, detail = 'fail', check_class(r.get('first_readable', '')), r.get('first_readable')
                    elif int(r.get('dead_v0', 0)):
                        # Declared void, but its v0 differs from the original's,
                        # which callers may read (zx-fncheck only notes it).
                        result, cls, detail = 'fail', 'void_v0', 'v0 differs in %s calls (declared void)' % r['dead_v0']
                    else:
                        result, cls, detail = 'pass', None, None
                    run.emit('check', k, route=route, result=result, calls=recorded, failed=bad + orig_bad,
                             first_failure=detail, cycles_equal=int(r.get('cycles_differ', 0)) == 0,
                             # what the check was made against (zx-fncheck's digests)
                             **{d: r[d] for d in ('impl', 'orig', 'rec', 'checker') if r.get(d) not in (None, '', '-')})
                else:
                    recorded = 0
                    result = 'fail' if r['result'] == 'fault' else r['result']
                    cls = r['result']
                    detail = r.get('detail')
                    run.emit('check', k, route=route, result=result, first_failure=detail)
                checks[route] = {'result': result, 'calls': recorded, 'class': cls, 'detail': (detail or '')[:200]}
                if cls and first_fail is None:
                    first_fail = (cls, '%s: %s' % (route, detail or ''))
            f['checks'] = checks
            f['stage'] = 'checked'
            if first_fail:
                run.failure(k, 'check', first_fail[0], first_fail[1])
                counts['fail'] += 1
            else:
                f.pop('failure', None) if f.get('failure', {}).get('stage') == 'check' else None
                counts['pass' if any(c['result'] == 'pass' for c in checks.values()) else 'no_recording'] += 1
            if (n + 1) % 50 == 0:
                run.save()
                log('checked %d of %d (%.0f s)' % (n + 1, len(futures), time.time() - t0))
    run.save()
    log('check: %s (%.0f s)' % (dict(counts), time.time() - t0))


# ---------------------------------------------------------------------------
# register
# ---------------------------------------------------------------------------

def cmd_demote(run):
    """Functions identical on every recorded call that still change the game
    (a logiccheck, a boot with Tested on: found by bisection) stay Draft: the
    failure is kept (stage register, the --why class) and register leaves them."""
    keys = run.selected(run.args.only)
    for k in keys:
        f = run.functions[k]
        f['demoted'] = run.args.why or 'game'
        run.failure(k, 'register', 'game:' + (run.args.why or 'game'), 'identical on its recordings; %s with it on' % (
            run.args.why or 'the game differs'))
    run.save()
    cmd_register(run)


def cmd_register(run):
    keys = run.selected(run.args.only)
    counts = collections.Counter()
    for k in keys:
        f = run.functions[k]
        if f.get('status') == 'Planned' or not f.get('span'):
            if f.get('status') == 'Planned':
                run.emit('status', k, status='Planned', registered=True, file=f['file'])
                counts['Planned'] += 1
            continue
        checks = f.get('checks') or {}
        recorded = [c for c in checks.values() if c['result'] != 'no_recording']
        tested = bool(recorded) and all(c['result'] == 'pass' for c in recorded) and not f.get('demoted')
        want = 'Tested' if tested else 'Draft'
        if tested and f.get('status') == 'Verified':
            want = 'Verified'  # promoted since (manifest-verify): stays
        f['status'] = want
        f['stage'] = 'registered'
        note = None
        if want == 'Draft':
            fail = f.get('failure')
            note = ('%s %s' % (fail['stage'], fail['class'])) if fail else 'no recording'
        run.emit('status', k, status=want, registered=True, file=f['file'], note=note)
        counts[want] += 1
    run.save()
    write_manifests(run)
    log('register: %s (in the manifests; rebuild to register them)' % dict(counts))


# ---------------------------------------------------------------------------
# the committed manifests alone
# ---------------------------------------------------------------------------

MANIFEST_ROOTS = ('zge/sweep', 'sce/sweep', 'aggressive/sweep')
DEMOTED = 'demoted: game:'
# Flags of functions no game path reaches (triggers1's scan of the ELF, its
# `trigger` notes in the work database: `dead`, no caller and
# no pointer in data or code, or only called from such code; `port-dead`,
# reached only from code the port replaces or never runs). Their status stays
# (the drafts build and are checked as before); the counts leave them out.
# `script-dead`: only reached from level-script natives no shipped script calls
# (paths3: every .BIN on the disc disassembled), given with --only/--why.
DEAD_FLAGS = {'dead', 'port-dead', 'script-dead'}
# `data-dead` (paths5): no AI PS2 data reaches it, but another release's data does (DM2 or BMX XXX
# on PS2, Xbox or GameCube, or AI's own Xbox/GameCube data: the reason column names the game and
# file). Not in DEAD_FLAGS: such rows stay targets (the PC game hosts every game's content).
KEPT_FLAGS = DEAD_FLAGS | {'data-dead', 'srcref'}


def addresses_of(text):
    """ADDR,... or @FILE (one address a line, `#` comments) as 0x%08X keys."""
    items = []
    for t in (text or '').split(','):
        t = t.strip()
        if t.startswith('@'):
            for line in open(os.path.expanduser(t[1:])):
                line = line.split('#')[0].strip()
                if line:
                    items.append(line.split()[0])
        elif t:
            items.append(t)
    return {'0x%08X' % int(a, 16) for a in items}


def cmd_manifest_demote(args):
    import readable_draft as rd
    want = addresses_of(args.only)
    if not want:
        sys.exit('readable_sweep.py manifest-demote: --only ADDR,... or @FILE')
    why = args.why or 'game'
    seen = set()
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if not os.path.exists(path):
            continue
        header, entries = rd.read_manifest(path)
        changed = 0
        for e in entries:
            k = '0x%08X' % e['address']
            if k not in want:
                continue
            seen.add(k)
            if args.promote:
                # A demotion taken back, or with --drafts any Draft (one set
                # aside whole, checked again since: sweepfix's 445).
                if e['status'] == 'Draft' and ((e['reason'] or '').startswith(DEMOTED) or
                                               (args.drafts and e['reason'] in ('', '-'))):
                    e['status'], e['reason'] = 'Tested', ''
                    changed += 1
            elif e['status'] == 'Tested':
                e['status'], e['reason'] = 'Draft', DEMOTED + why
                changed += 1
        if changed:
            rd.write_manifest(path, header, entries)
        log('%s: %d %s' % (ztree.key_of(path, GAME_SRC), changed, 'promoted' if args.promote else 'demoted'))
    missing = sorted(want - seen)
    if missing:
        log('not in a manifest: ' + ', '.join(missing))


def redraft_rows(rd, ws, root, path_rows, want):
    """Drafts `want` (rows of one batch file) as the build will; each row becomes Draft with its
    registration, or Planned with the reason it cannot be drafted. Returns the number drafted."""
    import argparse as ap
    ns = path_rows[0]['namespace']
    io = {e['address'] for e in want if 'io' in e['flags']}
    args = ap.Namespace(name=None, status='Draft', no_ghidra=True, namespace=ns, workdb=None, stable=True, io_dynamic=io)
    ws.elf_log, ws.elf_by_fn = {}, {}
    text, drafts, failures = rd.draft_batch(ws, [e['address'] for e in want], args, skip_failures=True)
    ws.drafting = None
    elf = ws.elf_by_fn
    ws.elf_log = ws.elf_by_fn = None
    by = {d.address: d for d in drafts}
    done = 0
    for e in want:
        a = e['address']
        d = by.get(a)
        why = failures.get(a)
        if d is not None and d.lifter.unsupported:
            why = not_lifted_class(d)
        if why or d is None:
            e['status'], e['macro'], e['reason'] = 'Planned', '', (why or 'not drafted')[:200]
            continue
        e['status'], e['macro'] = 'Draft', d.macro
        e['name'] = e.get('name') or d.name
        e['reason'] = e.get('keep_reason') or (e['reason'] if 'data-dead' in e['flags'] else '')
        e['elf'] = dict(e.get('elf') or {})
        e['elf'].update(elf.get('0x%08x' % a, {}))
        dec = ws.decomp(a)
        m = dec and re.search(r'^(.*?)\b(\w+)\s*\(([^)]*)\)\s*\n\s*\{', dec[1], re.S | re.M)
        if m:
            e['signature'] = '%s %s(%s)' % (m.group(1).strip().split('\n')[-1].strip(), m.group(2), ' '.join(m.group(3).split()))
        done += 1
    return done


def cmd_manifest_redraft(args):
    """Drafts rows of the committed manifests again, with no run state: every Planned row, or the
    --only list (Planned rows and, with --add, functions in no manifest yet: the hooked functions
    select leaves out; each gets the `held:` reason in --why, so promotable leaves it until
    someone has looked at the hook). Rows that draft become Draft; the rest stay Planned with
    the reason. With --build, the drafts that do not compile go back to Planned with the error
    (as the sweep's build stage) until the build's --target compiles."""
    import readable_draft as rd
    want = addresses_of(args.only) if args.only else None
    manifests = {}
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if os.path.exists(path):
            manifests[root] = (path,) + rd.read_manifest(path)
    rows = {'0x%08X' % e['address']: e for _, _, entries in manifests.values() for e in entries}
    picked = [k for k, e in rows.items() if e['status'] == 'Planned' and (want is None or k in want)]
    planned_only = sum(1 for k in (want or ()) if k in rows and rows[k]['status'] != 'Planned')
    if planned_only:
        log('%d of the list are not Planned: left as they are' % planned_only)
    new = sorted(k for k in (want or ()) if k not in rows)
    if new and not args.add:
        sys.exit('readable_sweep.py manifest-redraft: in no manifest (--add takes them): ' + ', '.join(new[:10]))
    rd_, ws = workspace()
    if rd.readable_names:
        rd.readable_names.state()  # (first: its later caches take a filled state for granted)
    if new:
        info = {int(r['addr']): r for r in workdb_query(
            "SELECT addr, layer, coalesce(module, '') AS module FROM ai_functions WHERE addr IN (%s)" % ', '.join(str(int(k, 16)) for k in new))}
        for layer in ('engine', 'game', 'sdk'):
            members = [k for k in new if info.get(int(k, 16), {}).get('layer') == layer]
            root = DEFAULT_ROOTS[layer]
            for n in range(0, len(members), PER_FILE):
                chunk = members[n:n + PER_FILE]
                stem = 'hooked_%08x' % int(chunk[0], 16)
                for k in chunk:
                    m = info[int(k, 16)]['module']
                    e = {'address': int(k, 16), 'status': 'Planned', 'file': os.path.join(root, stem + '.cpp'), 'namespace': 'sweep::' + stem,
                         'macro': '', 'flags': {'io'} if m in IO_MODULES or (layer == 'sdk' and not m) else set(),
                         # (the names table's, as select's rows get theirs: a row keeps a function's name to itself)
                         'name': rd.readable_names.state()['unswept'].get(int(k, 16), '') if rd.readable_names else '',
                         'signature': '', 'elf': {}, 'reason': '', 'keep_reason': (HELD + args.why) if args.why else ''}
                    rows[k] = e
                    manifests[root][2].append(e)
                    picked.append(k)
    by_file = collections.defaultdict(list)
    for k in picked:
        by_file[rows[k]['file']].append(rows[k])
    done = 0
    for file, want_rows in sorted(by_file.items()):
        done += redraft_rows(rd, ws, os.path.dirname(file), want_rows, want_rows)
    still = collections.Counter(rows[k]['reason'].split(':')[0] + ':' + rows[k]['reason'].split(':')[-1] for k in picked if rows[k]['status'] == 'Planned')
    log('redraft: %d rows, %d drafted, %d stay Planned: %s' % (len(picked), done, len(picked) - done, still.most_common(12)))

    def write():
        for root, (path, header, entries) in manifests.items():
            rd.pin_data_refs(ws, [e for e in entries if '0x%08X' % e['address'] in picked])
            rd.write_manifest(path, header, entries)
    if picked:
        write()
    if not args.build:
        return
    build = os.path.abspath(args.build)
    by_key = {'0x%08x' % e['address']: e for e in rows.values()}
    run = argparse.Namespace(functions=by_key)
    for attempt in range(40):
        cmake(build)
        p = subprocess.run(['ninja', '-C', build, '-k', '0', '-j', str(args.build_jobs)] + (args.target or ['zx-fncheck']),
                           capture_output=True, text=True)
        out = p.stdout + p.stderr
        if p.returncode == 0:
            log('redraft: the build is clean after %d attempt(s)' % (attempt + 1))
            return
        hit, unmapped = draft_errors(run, out, build)
        hit = {k: m for k, m in hit.items() if by_key[k]['status'] == 'Draft'}
        if not hit:
            sys.exit('readable_sweep.py manifest-redraft: the build fails outside the Draft rows:\n' + out[-3000:])
        for k, msg in hit.items():
            by_key[k]['status'], by_key[k]['macro'], by_key[k]['reason'] = 'Planned', '', ('does not compile: ' + msg)[:200]
            log('does not compile: %s %s' % (k, msg[:120]))
        write()
    sys.exit('readable_sweep.py manifest-redraft: the build still fails after 40 attempts')


def cmd_manifest_status(args):
    import readable_draft as rd
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if not os.path.exists(path):
            continue
        _, entries = rd.read_manifest(path)
        live = [e for e in entries if not e['flags'] & DEAD_FLAGS]
        counts = collections.Counter(e['status'] for e in live)
        dead = collections.Counter(f for e in entries for f in e['flags'] & DEAD_FLAGS)
        demoted = collections.Counter(e['reason'][len(DEMOTED):] for e in entries
                                      if (e['reason'] or '').startswith(DEMOTED))
        held = sum(1 for e in entries if e['status'] == 'Tested' and (e['reason'] or '').startswith(HELD))
        # data-dead (paths5: another release's data reaches it) is a target: counted above, and here.
        data_dead = sum(1 for e in live if 'data-dead' in e['flags'])
        print('%s: %d functions, %s (%d of them used by another release\'s data only, data-dead); dead %d, dead in '
              'the port %d, unused by the shipped scripts %d (not counted); demoted %s; Tested held back from Verified %d' % (
                  root, len(entries), ', '.join('%d %s' % (counts[s], s) for s in ('Verified', 'Tested', 'Draft', 'Planned')),
                  data_dead, dead['dead'], dead['port-dead'], dead['script-dead'], dict(sorted(demoted.items())) or 'none',
                  held))


def cmd_manifest_pins(args):
    """The pinned ELF words (the `elf` column) on a row other than the
    function that reads them: a jump table's words are code addresses inside
    the function whose JR reads it (the generated function table's starts
    say which), and a data reference (`refs`) is the row's own address. Until
    drafter23 the sweep pinned a batch file's words on its first row, so a
    row deleted by a hand rewrite took the others' tables with it (gamehand10:
    fn_196568 held fn_1C4810's, whose draft then failed). --fix moves each
    word to its owner's row (the words stay one global map: drafts do not
    change)."""
    import bisect
    import readable_draft as rd
    _, ws = workspace()
    # (each generated function's own start, from its name: the table also maps
    # the resume points inside a function to it)
    starts = sorted(set(int(m.group(1), 16) for n in ws.table().values() if not n.startswith('entry_')
                        for m in [re.search(r'_0x([0-9a-f]+)$', n)] if m))
    def owner(word):
        i = bisect.bisect_right(starts, word) - 1
        return starts[i] if i >= 0 else None
    total = moved = 0
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if not os.path.exists(path):
            continue
        header, entries = rd.read_manifest(path)
        rows = {e['address']: e for e in entries}
        wrong = collections.defaultdict(list)
        for e in entries:
            prev = None  # (address, owner) of the word before, for a table's end
            for a, w in sorted((e.get('elf') or {}).items()):
                if 'refs' in e['flags'] and w == e['address']:
                    prev = None
                    continue
                o = owner(w)
                # The word that ended a table (0, or code of another function
                # with no row) was read with the table: its reader's.
                if prev and prev[0] == a - 4 and (o is None or o not in rows) and prev[1] in rows:
                    o = prev[1]
                if o is not None and o != e['address']:
                    wrong[(e['address'], o)].append((a, w))
                prev = (a, o)
        for (at, o), words in sorted(wrong.items()):
            total += len(words)
            print('%s: 0x%08X %s holds %d words of 0x%08X %s%s (0x%X..0x%X)' % (
                root, at, rows[at]['name'], len(words), o, rows[o]['name'] if o in rows else '(no row)',
                '' if o in rows else ': kept', words[0][0], words[-1][0]))
            if args.fix and o in rows:
                for a, w in words:
                    del rows[at]['elf'][a]
                    rows[o]['elf'] = dict(rows[o].get('elf') or {})
                    rows[o]['elf'][a] = w
                    moved += 1
        if args.fix and wrong:
            rd.write_manifest(path, header, entries)
    print('%d pinned words on another function\'s row%s' % (total, ', %d moved' % moved if args.fix else ''))


def cmd_manifest_dead(args):
    """Sets the dead-code flags from triggers1's triggers.tsv (columns address,
    status, ..., bucket: `dead` or `port-dead`); --undo clears them all.
    Only the flags column changes."""
    import readable_draft as rd
    want = {}
    if not args.undo:
        if not args.triggers:
            sys.exit('readable_sweep.py manifest-dead: --triggers FILE')
        for line in open(os.path.expanduser(args.triggers), encoding='utf-8'):
            c = line.rstrip('\n').split('\t')
            if len(c) > 8 and c[0].startswith('0x') and c[8] in DEAD_FLAGS:
                want[int(c[0], 16)] = c[8]
        # More from elsewhere: --only ADDR,...|@FILE with the flag in --why
        # (an @FILE line may give its own flag after the address:
        # `0x0015F220 dead`), so one call sets every list.
        if args.only:
            flag = args.why or 'dead'
            if flag not in DEAD_FLAGS:
                sys.exit('readable_sweep.py manifest-dead: --why one of %s' % ', '.join(sorted(DEAD_FLAGS)))
            for t in args.only.split(','):
                t = t.strip()
                lines = open(os.path.expanduser(t[1:])) if t.startswith('@') else [t]
                for line in lines:
                    c = line.split('#')[0].split()
                    if not c:
                        continue
                    own = c[1] if len(c) > 1 and c[1] in DEAD_FLAGS else flag
                    want.setdefault(int(c[0], 16), own)
    seen = set()
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if not os.path.exists(path):
            continue
        header, entries = rd.read_manifest(path)
        changed = collections.Counter()
        for e in entries:
            flags = (e['flags'] - DEAD_FLAGS) | ({want[e['address']]} if e['address'] in want else set())
            if e['address'] in want:
                seen.add(e['address'])
            if flags != e['flags']:
                changed[want.get(e['address'], 'cleared')] += 1
                e['flags'] = flags
        if changed:
            rd.write_manifest(path, header, entries)
        log('%s: %s' % (ztree.key_of(path, GAME_SRC), dict(changed) or 'unchanged'))
    missing = sorted(set(want) - seen)
    if missing:
        log('not in a manifest (%d): %s' % (len(missing), ', '.join('0x%08X' % a for a in missing[:20])))


# ---------------------------------------------------------------------------
# promotion: swept Tested to Verified (on by default)
# ---------------------------------------------------------------------------

AREA_OF_ROOT = {'zge/sweep': 'engine', 'aggressive/sweep': 'game', 'sce/sweep': 'sce'}
HELD = 'held: '
ASIDE_FILE = os.path.join(HERE, 'readable_promote_aside.txt')
PROFILE = os.path.join(WORKSPACE, 'recomp', 'traces', 'live-fnprof.csv')
GHIDRA_CSV = os.path.join(WORKSPACE, 'recomp', 'ghidra-functions.csv')
REG_AI = re.compile(r'(?<![A-Z0-9_])READABLE_FUNCTION(?:_WAITS|_VU0)*\(\s*(0x[0-9A-Fa-f]+)\s*,\s*[^,)]+?\s*,\s*(\w+)\s*\)')


def aside_list(path):
    """{0x%08X: why} of the promotion's set-aside file: an address a line, the
    reason after it; `# why` lines name the reason of the lines after them."""
    out, why = {}, 'set aside'
    if not path or not os.path.exists(path):
        return out
    for line in open(path):
        line = line.strip()
        if line.startswith('#'):
            if line.startswith('# why:'):
                why = line[len('# why:'):].strip()
            continue
        if line:
            a, _, rest = line.partition(' ')
            out['0x%08X' % int(a, 16)] = rest.strip() or why
    return out


def code_and_profile():
    """({start: bytes}, {start: self_ns}, total ns): Ghidra's functions (inner
    entry_ labels left out), each clipped at the next start, and the live-play
    profile (ZX_FNPROF, recomp/traces/live-fnprof.csv) with entries at inner
    addresses (a function resumed at an entry block) folded into the function."""
    import bisect
    import csv
    starts = sorted({int(r['Start'], 16): int(r['Size']) for r in csv.DictReader(open(GHIDRA_CSV))
                     if not r['Name'].startswith('entry_')}.items())
    size = {}
    for i, (a, n) in enumerate(starts):
        size[a] = max(4, min(n, starts[i + 1][0] - a)) if i + 1 < len(starts) else n
    keys = [a for a, _ in starts]
    ns, total = collections.Counter(), 0
    if os.path.exists(PROFILE):
        for r in csv.DictReader(open(PROFILE)):
            a, t = int(r['address'], 16), int(r['self_ns'])
            total += t
            i = bisect.bisect_right(keys, a) - 1
            if i >= 0 and a < keys[i] + size[keys[i]]:
                ns[keys[i]] += t
    return size, ns, total


def verified_now(rd):
    """Addresses on by default now: the hand-written Verified registrations of
    AI (src/game, the _IN forms of other games left out) and the manifests'."""
    out = set()
    for top, _, names in ztree.walk(GAME_SRC):
        for n in names:
            if n.endswith(('.cpp', '.h')):
                for m in REG_AI.finditer(readable_symbols.literal_view(open(os.path.join(top, n), errors='replace').read())):
                    if m.group(2) == 'Verified':
                        out.add(int(m.group(1), 16))
    for root in MANIFEST_ROOTS:
        if os.path.exists(manifest_path(root)):
            out |= {e['address'] for e in rd.read_manifest(manifest_path(root))[1] if e['status'] == 'Verified'}
    return out


def hand_registered():
    """{address: file} of AI's hand-written registrations (READABLE_FUNCTION*, any status) in src/game."""
    out = {}
    for top, _, names in ztree.walk(GAME_SRC):
        for n in names:
            if n.endswith(('.cpp', '.h')):
                path = os.path.join(top, n)
                for m in REG_AI.finditer(readable_symbols.literal_view(open(path, errors='replace').read())):
                    out.setdefault(int(m.group(1), 16), ztree.key_of(path, GAME_SRC))
    return out


def checked_functions(dirs):
    """Per address (AI's), over the fncheck_corpus.py run folders: the result
    lines summed, the markers lines' unmarked entry blocks (reached in some
    file, marked in none) and the --safepoints lines summed."""
    per = {}
    flags = set()
    for d in dirs:
        d = os.path.expanduser(d)
        try:
            flags |= set(json.load(open(os.path.join(d, 'run.json'))).get('args') or [])
        except (OSError, ValueError):
            pass
        for line in open(os.path.join(d, 'lines.jsonl')):
            f = json.loads(line)
            kind = f['kind']
            if kind == 'fault':
                m = re.search(r'(?:([a-z0-9]+)-)?([0-9a-f]{8})\.fnrec$', f['file'])
                if m and (m.group(1) or 'ai') == 'ai':
                    per.setdefault(int(m.group(2), 16), collections.Counter())['fault'] += 1
                continue
            if f.get('game', 'ai') != 'ai':
                continue
            e = per.setdefault(int(f['address'], 16), collections.Counter())
            if kind == 'result':
                e['files'] += 1
                e['readable_files'] += f.get('readable') == '1'
                for c in ('recorded', 'readable_ok', 'readable_bad', 'original_bad', 'cycles_differ', 'skipped',
                          'args_left', 'regs_left'):
                    e[c] += int(f.get(c) or 0)
            elif kind == 'markers':
                e['markers_files'] += 1
                e['order_bad'] += int(f.get('order_bad') or 0)
                for c, col in (('unmarked', 'unmarked_blocks'), ('marked', 'marked_blocks_at'),
                               ('unmarked_loops', 'unmarked_loops'), ('marked_loops', 'marked_loops_at')):
                    e.setdefault('_' + c, set())
                    e['_' + c] |= {int(x, 16) for x in (f.get(col) or '').split(',') if x and x != '-'}
            elif kind == 'safepoints':
                e['sp_files'] += 1
                for c in ('points', 'differ', 'count_differ', 'original_bad'):
                    e['sp_' + c] += int(f.get(c) or 0)
    return per, flags


def promotion_verdict(e, aside_why):
    """None when a swept Tested function qualifies for Verified, else the first
    reason it does not (the criterion: docs/READABLE.md, "Statuses")."""
    if aside_why:
        return 'set aside: ' + aside_why
    if not e or not e['files']:
        return 'not checked'
    if e['fault']:
        return 'fault'
    if not e['recorded'] or e['readable_files'] < e['files']:
        return 'no recorded call' if not e['recorded'] else 'readable off in a file'
    if e['original_bad']:
        return 'original replay differs'
    if e['readable_bad'] or e['cycles_differ'] or e['readable_ok'] + e['skipped'] < e['recorded']:
        return 'differs (cycles)' if e['cycles_differ'] else 'differs'
    if e['skipped']:
        return 'calls skipped'
    if not e['markers_files']:
        return 'no --markers line'
    if (e.get('_unmarked') or set()) - (e.get('_marked') or set()):
        return 'unmarked entry blocks'
    if (e.get('_unmarked_loops') or set()) - (e.get('_marked_loops') or set()):
        return 'unmarked loop heads'
    if e['order_bad']:
        return 'markers out of order'
    if not e['sp_files']:
        return 'no --safepoints line'
    if e['sp_differ'] or e['sp_count_differ']:
        return 'safe points differ'
    if e['sp_original_bad']:
        return 'safe points: original replay differs'
    return None


def open_items(rd, ws, wanted=None):
    """{address: [lines]} of the swept functions whose draft holds an open item (OPEN_ITEM: a TODO,
    an unmodelled value), drafted as the build drafts them; wanted: only those addresses' files.
    Hand-written sources with one go under their path."""
    if rd.readable_names:
        rd.readable_names.state()
    REG = re.compile(r'^\s*READABLE_\w+\((?:\w+\s*,\s*)?(0x[0-9A-Fa-f]+)\s*,')
    found = collections.defaultdict(list)
    by_file = collections.defaultdict(list)
    for root in MANIFEST_ROOTS:
        if os.path.exists(manifest_path(root)):
            for e in rd.read_manifest(manifest_path(root))[1]:
                by_file[e['file']].append(e)
    for file, entries in sorted(by_file.items()):
        if wanted is not None and not any(e['address'] in wanted for e in entries):
            continue
        text, spans, failures = rd.batch_text(ws, entries, 'gate')
        chunk = []
        for line in text.split('\n'):
            chunk.append(line)
            m = REG.match(line)
            if m:
                a = int(m.group(1), 16)
                found[a] += [l.strip() for l in chunk if rd.OPEN_ITEM.search(l)]
                chunk = []
    for root, _, names in ztree.walk(GAME_SRC):
        if os.sep + 'sweep' in root:
            continue
        for name in names:
            if name.endswith(('.cpp', '.h')):
                path = os.path.join(root, name)
                for n, line in enumerate(open(path, errors='replace'), 1):
                    if rd.OPEN_ITEM.search(line.split('//')[0]) or 'TODO' in line:
                        found[ztree.key_of(path, GAME_SRC)].append('%d: %s' % (n, line.strip()))
    return {k: v for k, v in found.items() if v}


def report_open_items(rd, items, statuses):
    """Prints the Verified functions an open item is in, by kind; returns their number."""
    kinds = collections.Counter()
    listed = []
    for k, lines in sorted(items.items(), key=lambda kv: str(kv[0])):
        if isinstance(k, str):
            listed.append(k)
            kinds['hand-written'] += 1
        elif statuses.get(k) == 'Verified':
            listed.append('0x%08X' % k)
            for l in set(re.sub(r'0x[0-9A-Fa-f]+|\d+', 'N', re.search(r'TODO[^*\n]*|\? \w+', l).group(0))[:48] for l in lines):
                kinds[l] += 1
    if listed:
        log('GATE: %d Verified or hand-written with an open item: %s' % (len(listed), ', '.join(listed[:12]) + (' ...' if len(listed) > 12 else '')))
        for kind, n in kinds.most_common():
            log('    %4d %s' % (n, kind))
    return len(listed)


def cmd_manifest_todos(args):
    """The gate alone: every swept function with an open item, by status (one line each with --show)."""
    import readable_draft as rd
    _, ws = workspace()
    items = open_items(rd, ws)
    statuses = {e['address']: e['status'] for root in MANIFEST_ROOTS if os.path.exists(manifest_path(root))
                for e in rd.read_manifest(manifest_path(root))[1]}
    print(dict(collections.Counter(statuses.get(k, 'hand-written') for k in items)))
    n = report_open_items(rd, items, statuses)
    for k, lines in sorted(items.items(), key=lambda kv: str(kv[0])):
        if isinstance(k, str) or statuses.get(k) == 'Verified':
            print('%s %s: %s' % (('0x%08X' % k) if not isinstance(k, str) else k, statuses.get(k, ''), lines[0][:120]))
    return 1 if n else 0


def cmd_promotable(args):
    """The swept Tested functions (with --drafts the Draft ones too: paths2's
    recordings reach Drafts that pass) that qualify for Verified, with their
    share of live-play time and code, by area; --out writes the list. A Draft
    demoted by the game (`demoted: game:` in the reason) or flagged dead is
    not a candidate."""
    import readable_draft as rd
    if not args.checked:
        sys.exit('readable_sweep.py promotable: --checked DIR[,DIR] (fncheck_corpus.py run --cycles --safepoints '
                 '--markers folders)')
    per, flags = checked_functions(args.checked.split(','))
    missing = {'--safepoints', '--markers'} - flags  # fncheck_corpus.py run always adds --cycles
    if missing:
        log('warning: the runs were not made with %s; the criterion needs them' % ' '.join(sorted(missing)))
    aside = aside_list(args.aside if args.aside is not None else ASIDE_FILE)
    size, ns, total_ns = code_and_profile()
    code_total = sum(b for a, b in size.items() if 0x00100000 <= a < 0x0022DB40)
    on_now = verified_now(rd)
    hand = hand_registered()
    areas = [a.strip() for a in (args.area or 'engine,game,sce').split(',') if a.strip()]
    rows, why_count = [], collections.defaultdict(collections.Counter)
    for root in MANIFEST_ROOTS:
        area = AREA_OF_ROOT[root]
        if area not in areas or not os.path.exists(manifest_path(root)):
            continue
        for e in rd.read_manifest(manifest_path(root))[1]:
            if e['status'] != 'Tested' and not (
                    args.drafts and e['status'] == 'Draft' and not e['flags'] & DEAD_FLAGS and
                    not (e['reason'] or '').startswith(DEMOTED)):
                continue
            k = '0x%08X' % e['address']
            held = (e['reason'] or '')[len(HELD):] if (e['reason'] or '').startswith(HELD) else ''
            why = promotion_verdict(per.get(e['address']), aside.get(k) or (('held: ' + held) if held else ''))
            if why is None and e['address'] in hand:
                why = 'registered by hand too (%s): its manifest row should go' % hand[e['address']]
            why_count[area][why or 'qualifies'] += 1
            rows.append(dict(addr=e['address'], area=area, name=e['name'] or 'fn_%X' % e['address'], why=why,
                             ns=ns.get(e['address'], 0), bytes=size.get(e['address'], 0), status=e['status']))
    # The gate: a draft with a TODO or an unmodelled value does not qualify (drafter26).
    gate_rd, gate_ws = workspace()
    items = open_items(rd, gate_ws, {r['addr'] for r in rows if r['why'] is None})
    for r in rows:
        if r['why'] is None and r['addr'] in items:
            r['why'] = 'an open item in the draft: ' + items[r['addr']][0][:90]
            why_count[r['area']]['qualifies'] -= 1
            why_count[r['area']]['an open item in the draft (TODO, an unmodelled value)'] += 1
    report_open_items(rd, open_items(rd, gate_ws), {e['address']: e['status'] for root in MANIFEST_ROOTS
                                                    if os.path.exists(manifest_path(root)) for e in rd.read_manifest(manifest_path(root))[1]})
    ok = [r for r in rows if r['why'] is None]
    pct = lambda n, d: 100.0 * n / d if d else 0.0
    print('swept %s: %d, qualify for Verified: %d (criterion: identical with EE cycles on every recorded call, '
          'no unmarked reached entry block or loop head, markers in order, no differing interruption, not set aside)'
          % ('Tested and Draft' if args.drafts else 'Tested', len(rows), len(ok)))
    if args.drafts:
        print('  of them Draft: %d, qualify %d' % (sum(1 for r in rows if r['status'] == 'Draft'),
                                                  sum(1 for r in ok if r['status'] == 'Draft')))
    for area in areas:
        sub = [r for r in rows if r['area'] == area]
        q = [r for r in sub if r['why'] is None]
        print('  %-6s Tested %4d, qualify %4d: live-play %6.3f %% (%.2f s), code %6.3f %% (%d bytes)' % (
            area, len(sub), len(q), pct(sum(r['ns'] for r in q), total_ns), sum(r['ns'] for r in q) / 1e9,
            pct(sum(r['bytes'] for r in q), code_total), sum(r['bytes'] for r in q)))
        for why, n in why_count[area].most_common():
            if why != 'qualifies':
                print('           %4d %s' % (n, why))
    now_ns = sum(ns.get(a, 0) for a in on_now)
    now_b = sum(size.get(a, 0) for a in on_now)
    add_ns = sum(r['ns'] for r in ok)
    add_b = sum(r['bytes'] for r in ok)
    print('on by default now: %d functions, live-play %.2f %%, code %.2f %%; with these: %d, live-play %.2f %%, '
          'code %.2f %%' % (len(on_now), pct(now_ns, total_ns), pct(now_b, code_total), len(on_now | {r['addr'] for r in ok}),
                            pct(now_ns + add_ns, total_ns), pct(now_b + add_b, code_total)))
    if args.show:
        for r in sorted(ok, key=lambda r: -r['ns'])[:args.show]:
            print('    0x%08X %-6s %-28s %8.3f ms %6d bytes' % (r['addr'], r['area'], r['name'][:28], r['ns'] / 1e6,
                                                               r['bytes']))
    if args.out:
        with open(os.path.expanduser(args.out), 'w') as out:
            out.write('# readable_sweep.py promotable --checked %s%s: %d swept functions that qualify\n' % (
                args.checked, ' --drafts' if args.drafts else '', len(ok)))
            out.write('# address area name live_ns bytes status\n')
            for r in sorted(ok, key=lambda r: r['addr']):
                out.write('0x%08X %s %s %d %d %s\n' % (r['addr'], r['area'], r['name'], r['ns'], r['bytes'], r['status']))
        if args.out_rest:
            with open(os.path.expanduser(args.out_rest), 'w') as out:
                for r in sorted(rows, key=lambda r: r['addr']):
                    if r['why'] is not None:
                        out.write('0x%08X %s %s # %s\n' % (r['addr'], r['area'], r['name'], r['why']))


def cmd_manifest_verify(args):
    """Tested to Verified in the manifests (--only ADDR,... or @FILE; with
    --drafts a listed Draft too, as promotable --drafts lists them); --undo
    takes it back (Verified to Tested, with --why kept as `held: WHY` in the
    reason column, which promotable then reports)."""
    import readable_draft as rd
    want = addresses_of(args.only)
    if not want:
        sys.exit('readable_sweep.py manifest-verify: --only ADDR,... or @FILE')

    # The reason column is this command's only for a hold (`held: WHY`); any
    # other reason stays (a data-dead row's evidence: which release's data
    # reaches the function; promote2 had to put them back; drafter25).
    def own(reason):
        return not reason or reason.startswith(HELD)

    def kept(reason, new=''):
        return new if own(reason) else reason
    # The gate: a draft with a TODO or an unmodelled value is not Verified (drafter26).
    refused = {}
    if not args.undo:
        gate_rd, gate_ws = workspace()
        items = open_items(rd, gate_ws, {int(k, 16) for k in want})
        refused = {'0x%08X' % a: lines[0] for a, lines in items.items() if not isinstance(a, str) and '0x%08X' % a in want}
        for k, line in sorted(refused.items()):
            log('refused, an open item in the draft: %s: %s' % (k, line[:110]))
        report_open_items(rd, items, {e['address']: e['status'] for root in MANIFEST_ROOTS if os.path.exists(manifest_path(root))
                                      for e in rd.read_manifest(manifest_path(root))[1]})
    want = want - set(refused)
    seen = set(refused)
    for root in MANIFEST_ROOTS:
        path = manifest_path(root)
        if not os.path.exists(path):
            continue
        header, entries = rd.read_manifest(path)
        changed = 0
        for e in entries:
            k = '0x%08X' % e['address']
            if k not in want:
                continue
            seen.add(k)
            if args.undo and args.drafts and e['status'] == 'Draft':
                # A Draft held back (its batch's culprit): stays Draft with
                # `held: WHY`, which promotable --drafts reports; no --why
                # takes the hold off.
                reason = (HELD + args.why) if args.why else ''
                if (e['reason'] or '') != reason and (not e['reason'] or e['reason'].startswith(HELD)):
                    e['reason'] = reason
                    changed += 1
            elif args.undo and e['status'] in ('Verified', 'Tested'):
                if e['status'] == 'Verified' or args.why:
                    e['status'], e['reason'] = 'Tested', kept(e['reason'], (HELD + args.why) if args.why else '')
                    changed += 1
            elif not args.undo and (e['status'] == 'Tested' or (args.drafts and e['status'] == 'Draft')):
                e['status'], e['reason'] = 'Verified', kept(e['reason'])
                changed += 1
        if changed:
            rd.write_manifest(path, header, entries)
        log('%s: %d %s' % (ztree.key_of(path, GAME_SRC), changed, ('back to Tested or held' if args.drafts else 'back to Tested') if args.undo else 'Verified'))
    missing = sorted(want - seen)
    if missing:
        log('not in a manifest: ' + ', '.join(missing[:20]) + (' ...' if len(missing) > 20 else ''))


# ---------------------------------------------------------------------------
# status
# ---------------------------------------------------------------------------

def cmd_status(run):
    fs = run.functions
    print('run %s: %d functions, root %s, generator %s' % (run.name, len(fs), run.state.get('root'), run.generator))
    for layer in sorted(set(f['layer'] for f in fs.values())):
        sub = [f for f in fs.values() if f['layer'] == layer]
        st = collections.Counter(f.get('status', 'selected') for f in sub)
        checked = [f for f in sub if f.get('checks') is not None]
        recorded = [f for f in checked if any(c['result'] != 'no_recording' for c in f['checks'].values())]
        print('  %-6s %4d: drafted %d, built %d, checked %d (recorded %d), %s' % (
            layer, len(sub), len([f for f in sub if f.get('span') or f.get('stage') == 'build_failed']),
            len([f for f in sub if f.get('span')]), len(checked), len(recorded),
            ', '.join('%s %d' % kv for kv in sorted(st.items()))))
    classes = collections.Counter((f['failure']['stage'], f['failure']['class']) for f in fs.values() if f.get('failure'))
    print('  failure classes (newest per function):')
    for (stage, cls), n in classes.most_common():
        print('    %5d  %-6s %s' % (n, stage, cls))


def cmd_classes(run):
    want = run.args.cls
    rows = collections.defaultdict(list)
    for k, f in sorted(run.functions.items()):
        if f.get('failure') and (not want or f['failure']['class'] == want):
            rows[f['failure']['class']].append((k, f.get('name'), f['failure']['detail']))
    for cls, items in sorted(rows.items(), key=lambda kv: -len(kv[1])):
        print('%s (%d)' % (cls, len(items)))
        for k, name, detail in items[:run.args.show]:
            print('   %s %-28s %s' % (k, name or '', detail[:150]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', choices=['select', 'draft', 'build', 'check', 'register', 'demote', 'all', 'status',
                                        'classes', 'manifest-demote', 'manifest-status', 'promotable',
                                        'manifest-verify', 'manifest-dead', 'manifest-pins', 'manifest-redraft', 'manifest-todos'])
    ap.add_argument('--run', help='the sweep run\'s name (its state and results; not for manifest-*)')
    ap.add_argument('--work', help='the run\'s folder (default $ZAXIS_SCRATCH/sweep/<run>)')
    ap.add_argument('--results', help='result files\' folder (default $ZAXIS_SCRATCH/workdb/results/<run>)')
    ap.add_argument('--agent', default=os.environ.get('SWEEP_AGENT', 'sweep'))
    ap.add_argument('--layer', default='engine', help='select: layers (engine, sdk, game), comma separated')
    ap.add_argument('--module', action='append', help='select: only these modules (repeatable)')
    ap.add_argument('--exclude', help='select: addresses to leave out, comma separated')
    ap.add_argument('--limit', type=int)
    ap.add_argument('--root', help='select: the batch files\' folder under src/game (default by layer)')
    ap.add_argument('--only', help='draft/build/check/register only these addresses (comma separated)')
    ap.add_argument('--failed', help='draft/check/...: only functions whose newest failure class starts with one of '
                    'these (comma separated): the sweep\'s loop, after a generator fix')
    ap.add_argument('--no-workdb', action='store_true', help='draft: no work database cards in the drafts')
    ap.add_argument('--commented', action='store_true', help='draft: also commented copies of the batch files (Ghidra\'s '
                    'decompilation, the work database\'s card) under <work>/commented, for reading')
    ap.add_argument('--build', help='the CMake build dir')
    ap.add_argument('--runner', action='store_true', help='build: the runner too (for recording)')
    ap.add_argument('--build-jobs', type=int, default=8)
    ap.add_argument('--add', action='store_true', help='manifest-redraft: --only may name functions in no manifest yet')
    ap.add_argument('--target', action='append', help='manifest-redraft: the build\'s targets (default zx-fncheck)')
    ap.add_argument('--corpus', help='check: the corpus version folder (default %s)' % CORPUS)
    ap.add_argument('--jobs', type=int, default=6, help='check: zx-fncheck processes at once')
    ap.add_argument('--timeout', type=int, default=900, help='check: seconds for one zx-fncheck process')
    ap.add_argument('--cls', help='classes: only this class')
    ap.add_argument('--why', help='demote: the failure class after game: (boot, logiccheck, ...)')
    ap.add_argument('--promote', action='store_true', help='manifest-demote: take the demotion back (Draft to Tested)')
    ap.add_argument('--drafts', action='store_true',
                    help='manifest-demote --promote: also Drafts that were never demoted (no reason), as Tested; '
                    'promotable: Draft rows are candidates too; manifest-verify: Draft to Verified too (with --undo --why: '
                    'the Draft held back, `held: WHY`)')
    ap.add_argument('--show', type=int, default=8)
    ap.add_argument('--checked', help='promotable: fncheck_corpus.py run folders (--cycles --safepoints --markers), '
                    'comma separated')
    ap.add_argument('--aside', help='promotable: the set-aside file (default tools/checks/readable_promote_aside.txt; '
                    'empty for none)')
    ap.add_argument('--area', help='promotable: engine, game, sce (comma separated; default all)')
    ap.add_argument('--out', help='promotable: write the qualifying functions here (one a line)')
    ap.add_argument('--out-rest', help='promotable: with --out, the Tested that do not qualify and why')
    ap.add_argument('--undo', action='store_true', help='manifest-verify: Verified back to Tested (with --why: held)')
    ap.add_argument('--triggers', help='manifest-dead: triggers1\'s triggers.tsv')
    ap.add_argument('--fix', action='store_true', help='manifest-pins: move the words to their owners\' rows')
    args = ap.parse_args()
    if args.command == 'promotable':
        return cmd_promotable(args)
    if args.command == 'manifest-verify':
        return cmd_manifest_verify(args)
    if args.command == 'manifest-demote':
        return cmd_manifest_demote(args)
    if args.command == 'manifest-status':
        return cmd_manifest_status(args)
    if args.command == 'manifest-dead':
        return cmd_manifest_dead(args)
    if args.command == 'manifest-pins':
        return cmd_manifest_pins(args)
    if args.command == 'manifest-redraft':
        return cmd_manifest_redraft(args)
    if args.command == 'manifest-todos':
        return cmd_manifest_todos(args)
    if not args.run:
        sys.exit('readable_sweep.py: --run NAME')
    run = Run(args)
    if args.command in ('build', 'check', 'all') and not args.build:
        sys.exit('readable_sweep.py: --build DIR')
    if args.command == 'select':
        cmd_select(run)
    elif args.command == 'draft':
        cmd_draft(run)
    elif args.command == 'build':
        cmd_build(run)
    elif args.command == 'check':
        cmd_check(run)
    elif args.command == 'register':
        cmd_register(run)
    elif args.command == 'demote':
        cmd_demote(run)
    elif args.command == 'all':
        cmd_draft(run)
        cmd_build(run)
        cmd_check(run)
        cmd_register(run)
    elif args.command == 'status':
        cmd_status(run)
    elif args.command == 'classes':
        cmd_classes(run)


if __name__ == '__main__':
    main()
