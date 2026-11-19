#!/usr/bin/env python3
"""Builds drafts and checks them against recordings (docs/READABLE.md,
"Drafts and the sweep").

    readable_draft_check.py --build DIR [--draft | --file F.cpp] [--corpus DIR | --rec DIR ...] FUNCTION ...

For each function (address or name):
  1. --draft: readable_draft.py writes its draft to
     src/game/drafts/draft_<address>.cpp (default); --file F.cpp: check a
     file of your own (a draft being worked on) instead.
  2. A function already registered elsewhere (a Verified one being drafted
     again, a READABLE_PLANNED line) has that registration line turned into
     a comment for the build, so the draft is the one registered.
  3. ps2xFnCheck is built (ninja -C DIR ps2xFnCheck; a reconfigure first
     when the draft is a new file), then run with --cycles --only <address>
     over the recordings: --rec folders, or every folder of the corpus
     index (readable_corpus.py) that covers the function.
  4. The registrations turned off are put back as they were, and the draft
     files written by --draft removed (--keep leaves them).

Reports per function: compiles or not (with the first errors), and the
calls identical over all calls recorded, with or without the same cycles.
--json writes the results as JSON.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
PS2RECOMP = ztree.TREE
GAME_SRC = ztree.GAME
DRAFTS = os.path.join(GAME_SRC, 'drafts')
import zaxis_paths  # noqa: E402  (tooling/paths.env: the workspaces, the data, the work database)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
import readable_symbols


def registrations(address):
    """(path, line index) of every registration of address in the sources."""
    out = []
    pat = re.compile(r'^\s*READABLE_(FUNCTION_WAITS|FUNCTION_VU0|FUNCTION|PLANNED)\(\s*0x0*%x\s*,' % address, re.I)
    for root, _, files in os.walk(GAME_SRC):
        if os.path.abspath(root).startswith(os.path.abspath(DRAFTS)):
            continue
        for name in files:
            if not name.endswith('.cpp'):
                continue
            path = os.path.join(root, name)
            # (names on the page: the literal view keeps the lines, the caller edits the file's own)
            lines = readable_symbols.literal_view(open(path, errors='replace').read()).split('\n')
            for i, line in enumerate(lines):
                if pat.match(line):
                    out.append((path, i))
    return out


def corpus_folders(corpus, address):
    index = os.path.join(corpus, 'index.json')
    if not os.path.exists(index):
        sys.exit('readable_draft_check.py: no %s (readable_corpus.py index)' % index)
    data = json.load(open(index))
    out = []
    key = '%08x' % address
    for route, info in data.get('routes', {}).items():
        if key in info.get('functions', {}):
            out.append(os.path.join(corpus, route))
    return out


def parse_check(text):
    r = {'replay': None, 'readable': None, 'recorded': None, 'cycles_same': None, 'differences': []}
    m = re.search(r'original replay: (\d+) of (\d+)', text)
    if m:
        r['replay'] = int(m.group(1))
        r['recorded'] = int(m.group(2))
    m = re.search(r'readable:\s+(\d+) of (\d+) identical', text)
    if m:
        r['readable'] = int(m.group(1))
    m = re.search(r'EE cycles:\s+(.*)', text)
    if m:
        r['cycles'] = m.group(1).strip()
        r['cycles_same'] = m.group(1).startswith('the same')
    r['differences'] = [l.strip() for l in text.split('\n') if re.match(r'^\s+\d+ differ', l)][:6]
    return r


def run_check(fncheck, address, folders, show=2):
    totals = {'replay': 0, 'readable': 0, 'recorded': 0, 'cycles_same_folders': 0, 'folders': 0, 'notes': []}
    for f in folders:
        p = subprocess.run([fncheck, '--cycles', '--show', str(show), '--only', '0x%08x' % address, f],
                           capture_output=True, text=True)
        out = p.stdout + p.stderr
        r = parse_check(out)
        fault = re.search(r'\[fncheck\] [^\n]*touched EE (0x[0-9a-f]+)[^\n]*', out)
        if fault:
            # The check stopped at a memory access outside the recording (a
            # wrong address, or a call answered out of step): a failure,
            # not a folder without calls.
            totals['folders'] += 1
            totals['faults'] = totals.get('faults', 0) + 1
            before = re.search(r'\[fncheck\] before it: ([^\n]*)', out)
            totals['notes'].append('%s: %s%s' % (os.path.basename(f), fault.group(0).strip(),
                                                 ('; ' + before.group(1)) if before else ''))
            continue
        if r['recorded'] is None:
            continue
        totals['folders'] += 1
        totals['recorded'] += r['recorded']
        totals['replay'] += r['replay'] or 0
        totals['readable'] += r['readable'] or 0
        if r.get('cycles') is None:
            totals['cycles_unrecorded_folders'] = totals.get('cycles_unrecorded_folders', 0) + 1
        elif r.get('cycles_same'):
            totals['cycles_same_folders'] += 1
        elif r.get('cycles'):
            totals['notes'].append('%s: cycles %s' % (os.path.basename(f), r['cycles']))
        for dline in r['differences']:
            totals['notes'].append('%s: %s' % (os.path.basename(f), dline))
        if len(totals['notes']) < 12:
            first = [l.strip() for l in out.split('\n') if l.strip().startswith('call ')][:show]
            totals['notes'] += ['%s: %s' % (os.path.basename(f), l) for l in first]
    return totals


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('functions', nargs='+')
    ap.add_argument('--build', required=True, help='the CMake build dir (out/build)')
    ap.add_argument('--draft', action='store_true', help='draft each function (readable_draft.py)')
    ap.add_argument('--file', help='a C++ file with the version to check')
    ap.add_argument('--rec', action='append', default=[], help='a recording folder (repeatable)')
    ap.add_argument('--corpus', help='a corpus version folder (readable_corpus.py); default, when no --rec is '
                    'given: $ZAXIS_SCRATCH/reccorpus/v5')
    ap.add_argument('--keep', action='store_true', help='leave the drafts in src/game/drafts')
    ap.add_argument('--json')
    ap.add_argument('--no-rebuild', action='store_true', help='leave ps2xFnCheck built with the drafts')
    ap.add_argument('--show', type=int, default=2)
    ap.add_argument('--no-workdb', action='store_true', help='drafts without the work database\'s card')
    args = ap.parse_args()
    if not args.corpus and not args.rec:
        args.corpus = os.path.join(SCRATCH_ROOT, 'reccorpus', 'v5')

    import readable_draft as rd
    workspace = zaxis_paths.get('AI_WORKSPACE')
    generated = os.environ.get('PS2X_RUNNER_GENERATED_DIR') or os.path.join(workspace, 'recomp', 'generated')
    cache = os.path.join(args.build, 'CMakeCache.txt')
    if os.path.exists(cache):
        m = re.search(r'^PS2X_RUNNER_GENERATED_DIR:PATH=(.*)$', open(cache).read(), re.M)
        if m and m.group(1):
            generated = m.group(1)
    ws = rd.Workspace(workspace, generated, os.path.join(zaxis_paths.get('DM2_WORKSPACE'), 'ai-decomp.c'))
    addresses = [rd.resolve_address(ws, f) for f in args.functions]
    rd.BY_ADDRESS.update(addresses)

    written = []
    disabled = {}   # path -> original text
    results = {}
    aside = []      # other drafts in src/game/drafts, out of this build
    import shutil
    import tempfile
    aside_dir = tempfile.mkdtemp(prefix='draft-aside-')
    try:
        new_files = False
        if os.path.isdir(DRAFTS):
            keep = {os.path.abspath(args.file)} if args.file else set()
            for name in os.listdir(DRAFTS):
                path = os.path.join(DRAFTS, name)
                if os.path.abspath(path) in keep:
                    continue
                if args.draft and name in ['draft_%06x.cpp' % a for a in addresses]:
                    continue
                shutil.move(path, os.path.join(aside_dir, name))
                aside.append(name)
                new_files = True
        if args.draft:
            os.makedirs(DRAFTS, exist_ok=True)
            for a in addresses:
                path = os.path.join(DRAFTS, 'draft_%06x.cpp' % a)
                ns = argparse.Namespace(name=None, status='Draft', no_ghidra=False, namespace='draft_%06x' % a,
                                        workdb=None if args.no_workdb else rd.DEFAULT_WORKDB)
                try:
                    text, drafts = rd.draft_text(ws, [a], ns)
                except rd.Unsupported as e:
                    results[a] = {'compiles': False, 'error': 'not drafted: %s' % e}
                    continue
                if not os.path.exists(path):
                    new_files = True
                open(path, 'w').write(text)
                written.append(path)
                d = drafts[0]
                results[a] = {'name': d.name, 'instructions': len(d.lifter.insns), 'calls': len(d.lifter.calls),
                              'gotos': len(d.goto_targets), 'not_lifted': len(d.lifter.unsupported),
                              'todo': sorted(set(d.todo)), 'file': path}
        elif args.file:
            for a in addresses:
                results[a] = {'file': args.file}
        for a in addresses:
            for path, i in registrations(a):
                if args.file and os.path.abspath(path) == os.path.abspath(args.file):
                    continue
                if path not in disabled:
                    disabled[path] = open(path).read()
                lines = open(path).read().split('\n')
                lines[i] = '// (readable_draft_check.py: off for the draft) ' + lines[i]
                open(path, 'w').write('\n'.join(lines))
        if new_files or args.draft:
            subprocess.run(['cmake', args.build], capture_output=True, text=True)
        # Build; drafts that do not compile are set aside (and their
        # functions' registrations put back) so the others can be checked.
        for attempt in range(len(addresses) + 1):
            b = subprocess.run(['ninja', '-C', args.build, '-j', os.environ.get('NINJA_JOBS', '8'), 'ps2xFnCheck'], capture_output=True, text=True)
            build_ok = b.returncode == 0
            log = b.stdout + b.stderr
            failed = []
            for a in addresses:
                r = results.setdefault(a, {})
                if 'error' in r or r.get('compiles') is False:
                    continue
                f = r.get('file')
                errs = [l for l in log.split('\n') if f and os.path.basename(f) in l and 'error' in l]
                r['compiles'] = not errs
                r['errors'] = errs[:8]
                if errs:
                    failed.append(a)
            if build_ok or not failed:
                break
            for a in failed:
                f = results[a]['file']
                if f in written:
                    os.remove(f)
                    written.remove(f)
                for path, text in list(disabled.items()):
                    lines = open(path).read().split('\n')
                    for i, line in enumerate(lines):
                        if line.startswith('// (readable_draft_check.py: off for the draft) ') and \
                                re.search(r'\(\s*0x0*%x\s*,' % a, line, re.I):
                            lines[i] = line[len('// (readable_draft_check.py: off for the draft) '):]
                    open(path, 'w').write('\n'.join(lines))
            subprocess.run(['cmake', args.build], capture_output=True, text=True)
        if not build_ok:
            print(log[-3000:])
        if build_ok:
            fncheck = os.path.join(args.build, 'ps2xRuntime', 'ps2xFnCheck')
            for a in addresses:
                folders = list(args.rec)
                if args.corpus:
                    folders += corpus_folders(args.corpus, a)
                r = results[a]
                r.update(run_check(fncheck, a, folders, args.show))
    finally:
        for path, text in disabled.items():
            open(path, 'w').write(text)
        for name in aside:
            shutil.move(os.path.join(aside_dir, name), os.path.join(DRAFTS, name))
        os.rmdir(aside_dir)
        if not args.keep:
            for path in written:
                os.remove(path)
            if os.path.isdir(DRAFTS) and not os.listdir(DRAFTS):
                os.rmdir(DRAFTS)
            if written or aside:
                subprocess.run(['cmake', args.build], capture_output=True, text=True)
        if (written or disabled) and not args.no_rebuild:
            # ps2xFnCheck as the sources are again (else it keeps the drafts).
            subprocess.run(['ninja', '-C', args.build, '-j', os.environ.get('NINJA_JOBS', '8'), 'ps2xFnCheck'], capture_output=True, text=True)
    for a in addresses:
        r = results.get(a, {})
        name = r.get('name', '')
        if not r.get('compiles'):
            print('0x%06X %s: does not compile%s' % (a, name, (': ' + r['error']) if r.get('error') else ''))
            for e in r.get('errors', []):
                print('    ' + e)
            continue
        print('0x%06X %s: compiles; %d of %d recorded calls identical (%d folders, cycles the same in %d%s)%s' % (
            a, name, r.get('readable', 0), r.get('recorded', 0), r.get('folders', 0), r.get('cycles_same_folders', 0),
            (', not recorded in %d' % r['cycles_unrecorded_folders']) if r.get('cycles_unrecorded_folders') else '',
            '' if r.get('replay') == r.get('recorded') else '; original replay %s' % r.get('replay')) +
            ('; STOPPED by a memory fault in %d folders' % r['faults'] if r.get('faults') else ''))
        for t in r.get('todo', []):
            print('    TODO ' + t)
        for n in r.get('notes', [])[:10]:
            print('    ' + n)
    if args.json:
        json.dump({'%06x' % a: results.get(a, {}) for a in addresses}, open(args.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
