#!/usr/bin/env python3
"""system_status.py: how near each system is to done, per game, from the tools' own data.

    system_status.py [--json FILE] [--build DIR] [--bind DIR] [--no-workdb] [--top N] [--functions]

A system is a subsystem folder of the readable code (its key's first two folders), keyed as scaffolding.py and readable_layering.py key them (the
first two path parts: zge/render, aggressive/skater, dm2/rider; a game folder's top-level files are `aggressive`,
`zge`, `dm2`, `bmx`; `sce` is the libraries). One row per system, with for each game (ai, dm2, bmx):

  functions     the system's functions that game has: AI's registrations in the folder; in DM2 and BMX XXX the shared
                bodies at level 1 or 2 (readable_symbols.py headers over the bind tables), their own registrations
                (their folders, and READABLE_*_ONLY in shared files), and AI's registered functions whose counterpart
                (workdb counterparts, high or medium) the bind table does not reach (`unbound`: generated there)
  status        Verified / Tested / Draft (the registration, or game/<game>/symbols_status.tsv, Draft without a row)
                and generated (level 1, READABLE_PLANNED, a Planned row, or unbound)
  shape         per-game difference, per function: one body (level 2, no game test in it), small difference (level 2
                with kIs*, perGame, READABLE_PARAM/RESULT/RETURN_*, #if ZX_GAME, if constexpr in the body), own
                body (READABLE_*_OWN), version only (READABLE_*_ONLY), game own (a game folder's function with no
                registered AI counterpart), game copy (a game folder's function whose AI counterpart is registered and
                the pair identical, same or close), game drifted (the same with a drifted or weaker pair)
  readable      the readable lint's verdict per function (readable_lint.py: every rule passes)
  held unknown  (--build) call sites where the held-register table gives the entry value
                (fncheck_build/readable_held_unknown.tsv), per function of the system, by game
  no workdb name (workdb) the game's function has no best_names row in its build

and for the folder's code (comments left out, the framework ee/ not counted):

  scaffolding   scaffolding.py's kinds by folder (docs/SCAFFOLDING.md's find lines)
  layering      readable_layering.py's counts by folder (is_game, per_game, pp_game, own_body, ...)
  documented    functions with a comment above them (the lint's `documented`)
  probes        calls by address to a function that has a readable registration (readable_callee_by_address: a
                wrapper over ee::call / ee::game / game::call on its sym:: or a k-constant of it), resume points (points::F::x, here::x), guest-stack locals (ee::Local<), original tail calls
                (game::tail), calls through a symbol (ee::call / ee::game / game::call on sym:: or points::),
                address-shaped literals (the lint's guest range, a line saying `not an address` left out), padding
                members (`_0a4[N]`, and their bytes), `unknown` identifiers, layout checks (static_assert offsetof),
                and the lint's rules (registers, named, scaffold, calls) over all the folder's code, helpers and
                wrappers included, where the per-function lint sees only registered bodies

nearness = 100 x (Verified share over every game's functions) / (1 + scaffolding left per function), where
scaffolding left is scaffolding.py's counted kinds plus resume points. The counts only inform; they gate nothing.
"""
import argparse
import collections
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the tree; puts its tools/readable on sys.path)
try:   # the scratch folder: the tooling's own paths.env (and a workshop's own layout, when there)
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    import zpaths  # noqa: E402
    SCRATCH_ROOT = zpaths.ZAXIS_SCRATCH
except Exception:   # (a worker's copy of the checks alone: no tooling around them)
    SCRATCH_ROOT = os.path.expanduser('~')
import scaffolding  # noqa: E402
import readable_lint  # noqa: E402  (the tree's tools/readable)

GAME_SRC = ztree.TREE   # (its readable code: ztree.walk, key_of, readable)
TOOLS = ztree.TOOLS
GAMES = ('ai', 'dm2', 'bmx')
OTHER = ('dm2', 'bmx')
BUILD = {'ai': 'ai_ps2', 'dm2': 'dm2_ps2', 'bmx': 'bmx_ps2'}
GAME_INDEX = {'ai': 0, 'dm2': 1, 'bmx': 2}   # ee/game_id.h's Game
STATUS_ORDER = ('Verified', 'Tested', 'Draft', 'generated')
SHAPES = ('one body', 'small difference', 'own body', 'version only', 'game own', 'game copy', 'game drifted', 'address only',
          'unbound')
SAME_CLASSES = ('identical', 'same', 'close')   # a counterpart pair's classes that are the same logic (workdb counterparts)

GAME_TEST = re.compile(r'\bkIs(?:Dm2|Bmx|Ai)\b|\bkInstanceGame\b|\bperGame\s*\(|\bREADABLE_(?:PARAM|RESULT|RETURN)_(?:ONLY|EXCEPT)\b'
                       r'|^\s*#\s*if.*ZX_GAME|\bif\s+constexpr\b', re.M)
OWN = re.compile(r'^\s*READABLE_FUNCTION(?:_WAITS|_VU0)?_OWN\(([^)]*)\)', re.M)
PROBES = {
    'resume_points': re.compile(r'\bpoints::\w+::\w+|\bhere::\w+'),
    'guest_locals': re.compile(r'\bee::Local<'),
    'tail_calls': re.compile(r'\bgame::tail\s*\('),
    'symbol_calls': re.compile(r'\b(?:ee::(?:call|game|tailCall)|game::(?:call|tailCall))\s*(?:<[^;(]*?>)?\s*\(\s*(?:sym|points)::'),
    'unknown_names': re.compile(r'\b\w*[uU]nknown\w*'),
    'layout_checks': re.compile(r'\bstatic_assert\s*\(\s*offsetof\b'),
}
ADDRESS_CALL = re.compile(r'\b(?:ee::(?:call|game|tailCall)|game::(?:call|tailCall))\s*(?:<[^;(]*?>)?\s*\(\s*(sym::\w+|k\w+)')
ADDRESS_CONSTANT = re.compile(r'\bconstexpr\s+uint32_t\s+(k\w+)\s*=\s*sym::(\w+)\s*;')
PADDING = re.compile(r'\b_[0-9a-fA-F]{2,4}\s*\[\s*(0[xX][0-9a-fA-F]+|\d+)\s*\]')


def folder(rel):
    parts = rel.split('/')
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def layer(system):
    top = system.split('/')[0]
    return 'library' if top == 'sce' else 'engine' if top == 'zge' else 'game'


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, timeout=1800, **kw)


def sources():
    for top in ('zge', 'aggressive', 'dm2', 'bmx', 'sce'):
        for root, _, files in ztree.walk(GAME_SRC, top):
            for f in sorted(files):
                if f.endswith(('.cpp', '.h')):
                    path = os.path.join(root, f)
                    yield ztree.key_of(path, GAME_SRC).replace(os.sep, '/'), path


def code_of(raw):
    """The code with comments out; a line that says it is not an address keeps no literal."""
    lines = [re.sub(r'\b0[xX][0-9a-fA-F]+u?\b', '0', l) if 'not an address' in l else l for l in raw.split('\n')]
    return readable_lint.strip_comments('\n'.join(lines))


# ---- the folder's code ---------------------------------------------------------------------------------------

def probe_code(texts, registered):
    """{system: {probe: count}} over every source of the folder. REGISTERED: the names with a readable
    registration (AI's), for the calls by address to a function that has a body."""
    out = collections.defaultdict(collections.Counter)
    for rel, (raw, code) in texts.items():
        c = out[folder(rel)]
        constants = dict(ADDRESS_CONSTANT.findall(code))
        for m in ADDRESS_CALL.finditer(code):
            x = m.group(1)
            if (x[5:] if x.startswith('sym::') else constants.get(x)) in registered:
                c['readable_callee_by_address'] += 1
        for name, rx in PROBES.items():
            c[name] += len(rx.findall(code))
        for m in PADDING.finditer(code):
            c['padding_members'] += 1
            c['padding_bytes'] += int(m.group(1), 0)
        for hit in readable_lint.HEX.finditer(code):
            if readable_lint.guest_value(hit.group(1)):
                c['address_literals'] += 1
        for rule, table in (('lint_registers', readable_lint.REGISTERS), ('lint_named', readable_lint.NAMED),
                            ('lint_scaffold', readable_lint.SCAFFOLD), ('lint_calls', readable_lint.CALLS)):
            for pattern, _ in table:
                c[rule] += len(re.findall(pattern, code))
        c['files'] += 1
        c['lines'] += raw.count('\n')
    return out


def bodies(texts):
    """{(rel, name): body text} of every registration's body (readable_lint.body_of), and the own bodies
    {(game, AI name): (rel, own name)} of READABLE_*_OWN."""
    found, own = {}, {}
    for rel, (raw, code) in texts.items():
        for m in readable_lint.REGISTRATION.finditer(code):
            args = [a.strip() for a in m.group(3).split(',')]
            if m.group(2) == '_OWN':
                for game, name in zip(args[2::2], args[3::2]):
                    own[(game, args[0].split('::')[-1])] = (rel, name)
                    span = readable_lint.body_of(code, name, m.start())
                    if span:
                        found[(rel, name)] = code[span[0]:span[1]]
                names = [args[0]]
            else:
                names = [a.split('::')[-1] for a in args if re.match(r'^[A-Za-z_]\w*$', a.split('::')[-1])]
            for name in names:
                if name in ('Verified', 'Tested', 'Draft', 'Planned', 'dm2', 'bmx', 'ai'):
                    continue
                span = readable_lint.body_of(code, name, m.start())
                if span:
                    found[(rel, name)] = code[span[0]:span[1]]
        for m in readable_lint.ONLY_REGISTRATION.finditer(code):
            for name in [a.strip() for a in m.group(3).split(',')][:1]:
                span = readable_lint.body_of(code, name, m.start())
                if span:
                    found[(rel, name)] = code[span[0]:span[1]]
    return found, own


def planned(texts):
    """[(rel, name)] of READABLE_PLANNED registrations (recorded only: the generated code runs)."""
    out = []
    for rel, (raw, code) in texts.items():
        for m in readable_lint.REGISTRATION.finditer(code):
            if m.group(1) == 'PLANNED':
                args = [a.strip() for a in m.group(3).split(',')]
                out.append((rel, args[-1].split('::')[-1] if len(args) > 1 else args[0].split('::')[-1]))
    return out


# ---- the games ----------------------------------------------------------------------------------------------

def bind_rows(bind_dir, game):
    """{AI address: game address} of the bind table's function rows."""
    out = {}
    path = os.path.join(bind_dir, game + '.tsv')
    for line in open(path, errors='replace'):
        p = line.rstrip('\n').split('\t')
        if len(p) > 3 and p[0] in ('body', 'entry'):
            out[int(p[1], 16)] = int(p[3], 16)
    return out


def levels(game, bind_dir, tmp):
    """{AI name: (game address, level)} from readable_symbols.py headers --try-close (as the build and status.py)."""
    inst = [l.strip() for l in open(ztree.readable(GAME_SRC, game, 'instance_sources.txt')) if re.match(r'^[a-z]', l)]
    out = os.path.join(tmp, 'sym-' + game)
    r = run([sys.executable, '-B', os.path.join(TOOLS, 'readable_symbols.py'), 'headers', '--game', game,
             '--status', ztree.readable(GAME_SRC, game, 'symbols_status.tsv'), '--out', out, '--bind', os.path.join(bind_dir, game + '.tsv'),
             '--instance'] + inst + ['--try-close'], cwd=GAME_SRC)
    path = os.path.join(out, 'levels.tsv')
    if r.returncode or not os.path.exists(path):
        raise RuntimeError('levels for %s: %s' % (game, (r.stdout + r.stderr)[-300:]))
    rows = {}
    for line in open(path):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 3 and not line.startswith('#'):
            rows[p[0]] = (int(p[1], 16), int(p[2]))
    return rows


def statuses(game):
    rows = {}
    for line in open(ztree.readable(GAME_SRC, game, 'symbols_status.tsv'), encoding='utf-8'):
        p = line.split()
        if len(p) >= 2 and not line.startswith('#'):
            rows[p[0]] = p[1]
    return rows


def workdb(sql, python):
    r = run([python, '-I', os.path.join(ztree.TOOLING, 'workdb', 'workdb.py'), 'query', sql, '--json'])
    if r.returncode:
        raise RuntimeError('workdb: ' + (r.stderr or r.stdout)[-300:])
    return json.loads(r.stdout)


def held_unknown(build_dir):
    """{(game, function address): distinct sites} where the held table gives the entry value."""
    path = os.path.join(build_dir, 'runtime', 'fncheck_build', 'readable_held_unknown.tsv')
    sites = collections.defaultdict(set)
    for line in open(path):
        if line.startswith('#'):
            continue
        p = line.rstrip('\n').split('\t')
        if len(p) >= 4:
            sites[(int(p[0]), int(p[1], 16))].add(int(p[2], 16))
    return {k: len(v) for k, v in sites.items()}, path


def collect(a):
    errors = []
    texts = {}
    for rel, path in sources():
        raw = open(path, encoding='utf-8', errors='replace').read()
        texts[rel] = (raw, code_of(raw))
    body, own_bodies = bodies(texts)
    fns = []   # one row per (game, function)

    def add(game, name, rel, status, shape, addr=None, lines=0, readable=None, documented=None):
        fns.append({'game': game, 'name': name, 'file': rel, 'system': folder(rel), 'status': status, 'shape': shape,
                    'addr': addr, 'lines': lines, 'readable': readable, 'documented': documented})

    ai_rows = readable_lint.lint(GAME_SRC)
    ai_by_name = {}
    for r in ai_rows:
        add('ai', r['name'], r['file'], r['status'], 'baseline', int(r['addr'], 16) if r['addr'].startswith('0') else None,
            r['lines'], not r['fails'], r['documented'])
        ai_by_name[r['name']] = r
    for rel, name in planned(texts):
        if not rel.startswith(('dm2/', 'bmx/')):
            add('ai', name, rel, 'generated', 'baseline')
    ai_addr = {r['name']: int(r['addr'], 16) for r in ai_rows if r['addr'].startswith('0')}
    ai_name_of = {v: k for k, v in ai_addr.items()}

    counterparts = collections.defaultdict(dict)   # game: {game address: (AI address, class)}
    counterpart_of = collections.defaultdict(dict)  # game: {AI address: game address}
    best = collections.defaultdict(set)
    notes = collections.Counter()
    if not a.no_workdb:
        try:
            for r in workdb("SELECT build, ai_addr, addr, cls FROM counterparts WHERE build IN ('dm2_ps2', 'bmx_ps2') "
                            "AND NOT inlined AND confidence IN ('high', 'medium')", a.workdb_python):
                g = r['build'][:-4]
                counterparts[g][r['addr']] = (r['ai_addr'], r['cls'])
                counterpart_of[g].setdefault(r['ai_addr'], r['addr'])
            for r in workdb("SELECT build, addr FROM best_names WHERE build IN ('ai_ps2', 'dm2_ps2', 'bmx_ps2')", a.workdb_python):
                best[r['build'][:-4]].add(r['addr'])
            for r in workdb("SELECT build, addr, count(*) n FROM func_notes WHERE build IN ('ai_ps2', 'dm2_ps2', 'bmx_ps2') "
                            "GROUP BY ALL", a.workdb_python):
                notes[(r['build'][:-4], r['addr'])] = r['n']
        except (RuntimeError, OSError, ValueError) as e:
            errors.append(str(e))
            a.no_workdb = True

    with tempfile.TemporaryDirectory(prefix='system_status.') as tmp:
        for g in OTHER:
            try:
                lv = levels(g, a.bind, tmp)
                bound = bind_rows(a.bind, g)
            except (RuntimeError, OSError) as e:
                errors.append(str(e))
                continue
            st = statuses(g)
            ai_symbols = readable_lint.function_symbols('')
            own_rows = readable_lint.lint(GAME_SRC, (g, 'zge', 'sce', 'ee'), g)
            seen, seen_addr = set(), set()
            for r in own_rows:
                addr = r['addr']
                if addr.startswith('sym::'):
                    sym = addr[5:]
                    if sym not in ai_symbols or int(ai_symbols[sym], 16) not in bound:
                        continue   # (unbound in this game: the build does not register it here either)
                    addr = '0x%x' % bound[int(ai_symbols[sym], 16)]
                addr = int(addr, 16)
                in_folder = r['file'].startswith(g + '/')
                shape = 'version only'
                if in_folder:
                    shape = 'game own'
                    cp = counterparts[g].get(addr)
                    if cp and cp[0] in ai_name_of:
                        shape = 'game copy' if cp[1] in SAME_CLASSES else 'game drifted'
                add(g, r['name'], r['file'], r['status'], shape, addr, r['lines'], not r['fails'], r['documented'])
                seen.add(r['name'])
                seen_addr.add(addr)
            for rel, name in planned(texts):
                if rel.startswith(g + '/'):
                    add(g, name, rel, 'generated', 'game own')
                    seen.add(name)
            for name, (addr, level) in lv.items():
                if name in seen or level == 0 or name not in ai_by_name:
                    continue
                r = ai_by_name[name]
                if level == 1 or st.get(name) == 'Planned':
                    add(g, name, r['file'], 'generated', 'address only', addr, r['lines'])
                else:
                    if (g, name) in own_bodies:
                        shape = 'own body'
                    else:
                        text = body.get((r['file'], name), '')
                        shape = 'small difference' if GAME_TEST.search(text) else 'one body'
                    add(g, name, r['file'], st.get(name, 'Draft'), shape, addr, r['lines'], not r['fails'])
                seen.add(name)
                seen_addr.add(addr)
            for ai_a, game_a in counterpart_of[g].items():
                name = ai_name_of.get(ai_a)
                if name and name not in seen and game_a not in seen_addr and lv.get(name, (0, 0))[1] == 0:
                    add(g, name, ai_by_name[name]['file'], 'generated', 'unbound', game_a)
                    seen.add(name)

    held, held_path = ({}, None)
    if a.build:
        try:
            held, held_path = held_unknown(a.build)
        except OSError as e:
            errors.append('held table: %s' % e)
    for f in fns:
        f['held_unknown'] = held.get((GAME_INDEX[f['game']], f['addr']), 0) if f['addr'] is not None else 0
        f['workdb_name'] = None if a.no_workdb or f['addr'] is None else f['addr'] in best[f['game']]
        f['notes'] = notes.get((f['game'], f['addr']), 0) if f['addr'] is not None else 0

    scaff = scaffolding.count(GAME_SRC, scaffolding.kinds(os.path.join(ztree.DOCS, 'SCAFFOLDING.md')))
    lay_json = os.path.join(a.tmp_json_dir, 'layering.json') if a.tmp_json_dir else None
    layering = {}
    with tempfile.TemporaryDirectory(prefix='system_status.') as tmp:
        path = lay_json or os.path.join(tmp, 'layering.json')
        r = run([sys.executable, '-I', os.path.join(HERE, 'readable_layering.py'), '--root', GAME_SRC, '--json', path])
        if r.returncode == 0 and os.path.exists(path):
            layering = json.load(open(path)).get('folders', {})
        else:
            errors.append('readable_layering: ' + (r.stderr or r.stdout)[-300:])
    probes = probe_code(texts, {r['name'] for r in ai_rows})
    return summarize(fns, scaff, layering, probes, held_path, errors, a.no_workdb)


def summarize(fns, scaff, layering, probes, held_path, errors, no_workdb):
    systems = sorted(set(f['system'] for f in fns) | set(probes))
    rows = []
    for s in systems:
        mine = [f for f in fns if f['system'] == s]
        per = {}
        for g in GAMES:
            fg = [f for f in mine if f['game'] == g]
            st = collections.Counter(f['status'] for f in fg)
            per[g] = {
                'functions': len(fg),
                'status': {k: st.get(k, 0) for k in STATUS_ORDER},
                'shape': {k: v for k, v in collections.Counter(f['shape'] for f in fg).items()},
                'readable': sum(1 for f in fg if f['readable']),
                'documented': sum(1 for f in fg if f['documented']),
                'lines': sum(f['lines'] for f in fg),
                'held_unknown_functions': sum(1 for f in fg if f['held_unknown']),
                'held_unknown_sites': sum(f['held_unknown'] for f in fg),
                'no_workdb_name': None if no_workdb else sum(1 for f in fg if f['workdb_name'] is False),
                'with_notes': sum(1 for f in fg if f['notes']),
            }
        sc = {k: v.get(s, 0) for k, v in scaff.items()}
        pr = dict(probes.get(s, {}))
        slots = sum(per[g]['functions'] for g in GAMES)
        verified = sum(per[g]['status']['Verified'] for g in GAMES)
        size = max([per[g]['functions'] for g in GAMES] + [1])
        left = sum(sc.values()) + pr.get('resume_points', 0)
        share = verified / slots if slots else 0.0
        rows.append({
            'system': s, 'layer': layer(s), 'games': [g for g in GAMES if per[g]['functions']], 'per_game': per,
            'slots': slots, 'verified': verified, 'verified_share': round(share, 4),
            'scaffolding': sc, 'scaffolding_left': left, 'left_per_function': round(left / size, 3),
            'nearness': round(100.0 * share / (1.0 + left / size), 1),
            'layering': layering.get(s, {}), 'probes': pr,
        })
    rows.sort(key=lambda r: (-r['nearness'], -r['slots'], r['system']))
    return {'tree': ztree.TREE, 'commit': run(['git', '-C', ztree.TREE, 'rev-parse', '--short=9', 'HEAD']).stdout.strip(),
            'held_table': held_path, 'errors': errors, 'systems': rows, 'functions': fns}


def print_table(data, top):
    print('system status of %s (%s)' % (data['tree'], data['commit']))
    for e in data['errors']:
        print('  (missing: %s)' % e)
    head = '%-22s %-7s %6s %5s  %-17s %-17s %-17s %5s %5s %5s %5s %6s' % (
        'system', 'layer', 'near', 'V%', 'ai V/T/D/gen', 'dm2 V/T/D/gen', 'bmx V/T/D/gen', 'scaf', 'resum', 'rdadr',
        'pad', 'held')
    print(head)
    print('  (near: nearness; V%: Verified share over every game; scaf: scaffolding.py kinds; resum: resume points;'
          ' rdadr: calls by address to a function with a readable body; pad: padding members; held: functions with'
          ' held-table unknowns)')
    for r in data['systems'][:top or None]:
        cells = []
        for g in GAMES:
            p = r['per_game'][g]
            cells.append('/'.join(str(p['status'][k]) for k in STATUS_ORDER) if p['functions'] else '-')
        held = sum(r['per_game'][g]['held_unknown_functions'] for g in GAMES)
        print('%-22s %-7s %6.1f %5.1f  %-17s %-17s %-17s %5d %5d %5d %5d %6d' % (
            r['system'], r['layer'], r['nearness'], 100 * r['verified_share'], cells[0], cells[1], cells[2],
            sum(r['scaffolding'].values()), r['probes'].get('resume_points', 0), r['probes'].get('readable_callee_by_address', 0),
            r['probes'].get('padding_members', 0), held))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json', help='write every count (and every function row) here')
    ap.add_argument('--build', help="a build dir whose fncheck_build/readable_held_unknown.tsv to read (the tree's build)")
    # (the tree's own tables, tools/readable/bind since tools39; the shared folder for a tree from before)
    ap.add_argument('--bind', default=os.environ.get('ZX_BIND_DIR') or (os.path.join(ztree.TOOLS, 'bind') if os.path.isfile(
        os.path.join(ztree.TOOLS, 'bind', 'dm2.tsv')) else os.path.join(
        os.environ.get('ZAXIS_SCRATCH') or SCRATCH_ROOT, 'workdb', 'bind')))
    ap.add_argument('--no-workdb', action='store_true')
    ap.add_argument('--workdb-python', default=os.environ.get('WORKDB_PYTHON') or os.path.join(
        os.environ.get('ZAXIS_SCRATCH') or SCRATCH_ROOT, 'venv', 'bin', 'python'))
    ap.add_argument('--top', type=int, default=0)
    ap.add_argument('--tmp-json-dir', help=argparse.SUPPRESS)
    a = ap.parse_args()
    data = collect(a)
    print_table(data, a.top)
    if a.json:
        with open(a.json, 'w') as f:
            json.dump(data, f, indent=1, sort_keys=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
