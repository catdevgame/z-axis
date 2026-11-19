#!/usr/bin/env python3
"""Where each build's Ghidra exports are, as analysed (base) and from the readable copy of its
project (readable: the readable rewrite's names, signatures and structs applied, MATCH3.md
section 7). The tools that read decompilations or function exports take the readable ones by
default; `--base` (or READABLE_EXPORTS=base in the environment) gives the original ones.

    python3 -I tools/exports.py list                    each build's files, readable and base
    python3 -I tools/exports.py install BUILD WORKDIR   copy a readable pass's outputs
                                                        (WORKDIR/readable/) into the readable folder
    python3 -I tools/exports.py path BUILD decomp|export [--base]

Kinds: `decomp` (ExportDecomp.java's C, every build) and `export` (ExportXbe.java's jsonl, the
Xbox and GameCube builds; a readable one names the analysis name "n" and the readable one "rn",
so the matching tools and the work database read the same names as from the base file).

The readable folder (READABLE_EXPORTS_DIR, default $ZAXIS_SCRATCH/readable-exports) has
one folder per build: decomp.c, export.jsonl, and source.json (where the files came from and
their SHA-256). A build with no readable file there falls back to its base file with a warning
on stderr. Everything here is disc derived: scratch only, never committed.

A tool's argument that names a decompilation may be a build (`ai_ps2`, `dm2_xbox`, ...) instead
of a path: resolve(ARG, 'decomp', readable) turns it into the file.
"""
import hashlib, json, os, shutil, sys

HOME = os.path.expanduser('~')
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
import zpaths  # noqa: E402  (tooling/paths.env)
SCRATCH = os.environ.get('AI_SCRATCH', zpaths.ZAXIS_SCRATCH)
# where the new builds' Ghidra outputs live (not WORKDB_DIR: a scratch database built elsewhere
# still reads these)
WORKDB = SCRATCH + '/workdb'
DM2_REPO = os.environ.get('DM2_REPO', zpaths.DM2_WORKSPACE)
M3 = SCRATCH + '/match3'
READABLE_DIR = os.environ.get('READABLE_EXPORTS_DIR', SCRATCH + '/readable-exports')

# the analysed projects' own exports (what the tools read before the readable stage)
BASE = {
    'ai_ps2': {'decomp': DM2_REPO + '/ai-decomp.c'},
    'dm2_ps2': {'decomp': DM2_REPO + '/dm2-decomp.c'},
    'bmx_ps2': {'decomp': M3 + '/ghidra-bmxxxx/BmxXXX-decomp.c'},
    'ai_xbox': {'decomp': SCRATCH + '/aixbox/ghidra-aixbox/AiXbox-decomp.c',
                'export': SCRATCH + '/aixbox/ghidra-aixbox/out/AiXbox-xbe.jsonl'},
    'dm2_xbox': {'decomp': SCRATCH + '/dm2xbox-ghidra/ghidra/Dm2Xbox-decomp.c',
                 'export': SCRATCH + '/dm2xbox-ghidra/ghidra/out/Dm2Xbox-xbe.jsonl'},
    'bmx_xbox': {'decomp': WORKDB + '/bmxxbox/ghidra/BmxXbox-decomp.c',
                 'export': WORKDB + '/bmxxbox/ghidra/out/BmxXbox-xbe.jsonl'},
    'ai_gc': {'decomp': WORKDB + '/gc/ghidra-ai/AiGc-decomp.c',
              'export': WORKDB + '/gc/ghidra-ai/out/AiGc-gc.jsonl'},
    'dm2_gc': {'decomp': WORKDB + '/gc/ghidra-dm2/Dm2Gc-decomp.c',
               'export': WORKDB + '/gc/ghidra-dm2/out/Dm2Gc-gc.jsonl'},
    'bmx_gc': {'decomp': WORKDB + '/gc/ghidra-bmx/BmxGc-decomp.c',
               'export': WORKDB + '/gc/ghidra-bmx/out/BmxGc-gc.jsonl'},
}
BUILDS = tuple(BASE)
FILE = {'decomp': 'decomp.c', 'export': 'export.jsonl'}


def default_readable():
    return os.environ.get('READABLE_EXPORTS', 'readable') != 'base'


def base_flag(argv):
    """Removes --base from argv; True when the readable exports are wanted."""
    if '--base' in argv:
        argv.remove('--base')
        return False
    return default_readable()


def readable_path(build, kind):
    return os.path.join(READABLE_DIR, build, FILE[kind])


def path(build, kind, readable=None, warn=True):
    """The file of one kind for a build: the readable one (default) if it is there, else the base
    one (with a warning)."""
    if readable is None:
        readable = default_readable()
    base = BASE[build].get(kind)
    if readable:
        p = readable_path(build, kind)
        if os.path.exists(p):
            return p
        if warn and base:
            print('exports: no readable %s for %s (%s); using %s' % (kind, build, p, base), file=sys.stderr)
    return base


def resolve(arg, kind='decomp', readable=None):
    """A tool's argument: a build name becomes its file, a path stays."""
    return path(arg, kind, readable) if arg in BASE else arg


def sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''):
            h.update(b)
    return h.hexdigest()


def install(build, work):
    """Copies <work>/readable/<name>-decomp.c (and -xbe.jsonl / -gc.jsonl) into the folder."""
    src = os.path.join(work, 'readable')
    files = sorted(os.listdir(src))
    dec = [f for f in files if f.endswith('-decomp.c')]
    exp = [f for f in files if f.endswith(('-xbe.jsonl', '-gc.jsonl'))]
    if len(dec) != 1 or len(exp) > 1 or ('export' in BASE[build]) != bool(exp):
        sys.exit('exports: %s does not hold one decompilation (and, for %s, one export)' % (src, build))
    out = os.path.join(READABLE_DIR, build)
    os.makedirs(out, exist_ok=True)
    meta = {'build': build, 'work': os.path.abspath(work), 'files': {}}
    for kind, f in [('decomp', dec[0])] + ([('export', exp[0])] if exp else []):
        p = os.path.join(src, f)
        shutil.copyfile(p, readable_path(build, kind) + '.tmp')
        os.replace(readable_path(build, kind) + '.tmp', readable_path(build, kind))
        meta['files'][FILE[kind]] = {'from': p, 'sha256': sha256(p)}
        with open(p, errors='replace') as fh:
            first = fh.readline().strip()
        if first.startswith('// readable write-back'):
            meta['files'][FILE[kind]]['header'] = first
    json.dump(meta, open(os.path.join(out, 'source.json'), 'w'), indent=1, sort_keys=True)
    print('exports: %s <- %s' % (out, ', '.join(m['from'] for m in meta['files'].values())))


def main(argv):
    if not argv:
        sys.exit(__doc__)
    if argv[0] == 'list':
        for b in BUILDS:
            for kind in ('decomp', 'export'):
                if kind not in BASE[b]:
                    continue
                r = readable_path(b, kind)
                print('%-9s %-7s readable %s  base %s' % (
                    b, kind, r if os.path.exists(r) else '(none)',
                    BASE[b][kind] if os.path.exists(BASE[b][kind]) else '(missing) ' + BASE[b][kind]))
    elif argv[0] == 'install' and len(argv) == 3:
        install(argv[1], argv[2])
    elif argv[0] == 'path' and len(argv) >= 3:
        r = base_flag(argv)
        print(path(argv[1], argv[2], r))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv[1:])
