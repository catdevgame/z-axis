#!/usr/bin/env python3
"""proofrun: a target list of readable bodies played to systematically, recording only what the corpus lacks (proof1).

    proofrun.py targets GAME DEPTH_JSON OUT_TSV [--blocks BLOCKS_TSV] [--extra LIST_TSV ...] [--thin]
    proofrun.py plan GAME TARGETS_TSV OUT_JSONL [--seeds N] [--ticks T] [--top K]
    proofrun.py run GAME PLAN_JSONL STORE --seed-file SEED [--jobs 2] [--limit N] [--max-secs S]
    proofrun.py tally GAME STORE TARGETS_TSV

targets: the bodies with blocks no corpus recording ran (DEPTH_JSON: proof1's depth measure, from a replay's coverage
lines) and, with --thin, the recorded bodies with 3 calls or fewer or one route; --blocks (proof1's static sorting of the
unrun blocks: a TSV with game, function, name, block, bytes, class) counts only the `reachable` and `unsure` ones; --extra
adds a hand-made list (routes3's, address first). Each target keeps the routes that ran it.

plan: the game's own starting points, the level's start in each mode by its menus (dm2.py play, bmx.py play), ranked by
the unproven bytes of the targets a route on that level and mode has run (a body the corpus ran on every level counts on
each), each with N seeds of the closed-loop player (its choices differ by seed: the input variety).

run: each planned session played by the game's player with the target list recorded (ZX_FNREC=@list, the recorder's
ZX_FNREC_NEW_BLOCKS=1 seeded with the corpus's covered blocks, ZX_FNREC_COVERED=SEED: a call is kept only if it
runs a block no corpus file ran), readable off, unpaced, null GS, muted, a scratch HOME, at most 2 at once. Each session
into STORE/<game>-v1/<session>; a folder whose runs kept nothing is removed.

tally: per target, the blocks each session added (the recorder's [fncheck:newblocks] lines) and the sessions kept.
"""
import argparse
import collections
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))

# Level index (the players' level=N) by the corpus's route names
DM2_LEVELS = {'woodward': 0, 'trainyards': 1, 'swamp': 2, 'commercial': 3, 'galloon': 4, 'hwy47': 5, 'devils': 6,
              'airport': 7}
BMX_LEVELS = {'syra': 0, 'shep': 1, 'dam': 2, 'mnco': 3, 'muly': 4, 'nasa': 5, 'rmpg': 6, 'root': 7}
DM2_MODES = {'fr': 'freeride', 'pq': 'proquest', 'se': 'session'}


def where(game, route):
    """(mode, level) a route played, or None (front end, editor, menus, card, multiplayer)."""
    r = route.lower()
    if game == 'dm2':
        m = re.search(r'(?:^|-)(fr|pq|se)-?([a-z0-9]+?)(?:-t\d|-win|$)', r)
        if m and m.group(2) in DM2_LEVELS:
            return DM2_MODES[m.group(1)], DM2_LEVELS[m.group(2)]
        m = re.search(r'(?:^|-)g\d?-(fr|pq|se)(\d)$', r) or re.search(r'-p\d-(fr|pq|se)(\d)$', r)
        if m:
            return DM2_MODES[m.group(1)], int(m.group(2))
        return None
    for name, n in BMX_LEVELS.items():
        if re.search(r'(?:^|-)%s\d?(?:-|$)' % name, r):
            return 'challenge', n
    return None


def cmd_targets(a):
    depth = json.load(open(a.depth))[a.game]['bodies']
    reach = None
    if a.blocks:
        reach = collections.defaultdict(int)
        for line in open(a.blocks):
            p = line.rstrip('\n').split('\t')
            if len(p) < 6 or {'dm2': 'dm2', 'bmx xxx': 'bmx', 'bmx': 'bmx'}.get(p[0].lower()) != a.game:
                continue
            if p[5] in ('reachable', 'unsure'):
                reach[int(p[1], 16)] += int(p[4])
    rows = {}
    for b in depth:
        addr = int(b['addr'], 16)
        unrun = b['unproven'] if b['recorded'] else 0
        if reach is not None:
            unrun = reach.get(addr, 0)
        thin = b['recorded'] and (b['calls'] <= 3 or len(b['routes']) == 1)
        if unrun > 0 or (a.thin and thin):
            rows[addr] = (b['name'], unrun, int(thin), ','.join(b['routes']))
    for extra in a.extra or []:
        for line in open(extra):
            if line.startswith('#') or not line.strip():
                continue
            p = line.rstrip('\n').split('\t')
            addr = int(p[0], 16)
            if addr not in rows:
                rows[addr] = (p[1] if len(p) > 1 else '?', 1, 0, p[2] if len(p) > 2 else '')
    with open(a.out, 'w') as f:
        f.write('# address\tname\tunrun_bytes\tthin\troutes\n')
        for addr in sorted(rows):
            f.write('0x%08x\t%s\t%d\t%d\t%s\n' % ((addr,) + rows[addr]))
    print('%d targets, %d unrun bytes, %d thin' % (len(rows), sum(r[1] for r in rows.values()),
                                                     sum(r[2] for r in rows.values())))


def read_targets(path):
    out = []
    for line in open(path):
        if line.startswith('#') or not line.strip():
            continue
        p = line.rstrip('\n').split('\t')
        out.append({'addr': int(p[0], 16), 'name': p[1], 'bytes': int(p[2]), 'thin': p[3] == '1',
                    'routes': [r for r in p[4].split(',') if r] if len(p) > 4 else []})
    return out


def cmd_plan(a):
    targets = read_targets(a.targets)
    score = collections.Counter()
    levels = range(8)
    modes = ('freeride', 'proquest', 'session') if a.game == 'dm2' else ('challenge',)
    for t in targets:
        places = {where(a.game, r) for r in t['routes']} - {None}
        weight = t['bytes'] + (64 if t['thin'] else 0)
        if not places:
            continue
        for p in places:
            score[p] += weight
    plan = []
    for (mode, level), s in score.most_common(a.top):
        for seed in range(1, a.seeds + 1):
            plan.append({'game': a.game, 'mode': mode, 'level': level, 'seed': 100 + seed, 'ticks': a.ticks,
                         'score': s, 'name': '%s-%s%d-s%d' % ('pr', mode[:2], level, 100 + seed)})
    with open(a.out, 'w') as f:
        for p in plan:
            f.write(json.dumps(p) + '\n')
    print('%d sessions over %d places (top %d of %d)' % (len(plan), min(a.top, len(score)), a.top, len(score)))
    for (mode, level), s in score.most_common(a.top):
        print('  %-10s level %d: %d bytes' % (mode, level, s))


def cmd_run(a):
    targets = read_targets(a.targets)
    plan = [json.loads(l) for l in open(a.plan) if l.strip()][:a.limit or None]
    store = os.path.abspath(a.store)
    work = os.path.join(store, 'work')
    os.makedirs(work, exist_ok=True)
    listing = os.path.join(work, '%s-targets.txt' % a.game)
    with open(listing, 'w') as f:
        for t in targets:
            f.write('0x%08x 1000 8\n' % t['addr'])
    todo = list(plan)
    lock = threading.Lock()
    results = []

    def one(p):
        out = os.path.join(store, '%s-v1' % a.game, p['name'])
        shutil.rmtree(out, ignore_errors=True)
        os.makedirs(out)
        env = dict(os.environ)
        env.update({'ZX_FNREC': '@' + listing, 'ZX_FNREC_DIR': out, 'ZX_FNREC_ATOMIC': '1',
                    'ZX_FNREC_MAX': '1000', 'ZX_FNREC_PER_VSYNC': '8', 'ZX_FNREC_NEW_BLOCKS': '1',
                    'ZX_FNREC_COVERED': os.path.abspath(a.seed_file), 'ZX_AUDIO': 'off',
                    'SDL_AUDIODRIVER': 'dummy', 'SDL_VIDEODRIVER': 'dummy', 'ZX_HEADLESS': '1'})
        if a.game == 'dm2':
            cmd = [sys.executable, os.path.join(HERE, 'dm2.py'), 'play', p['name'], 'mode=' + p['mode'],
                   'level=%d' % p['level'], 'seed=%d' % p['seed'], 'ticks=%d' % p['ticks']]
        else:
            cmd = [sys.executable, os.path.join(HERE, 'bmx.py'), 'play', p['name'], str(p['level']), str(p['seed']),
                   str(p['ticks'])]
        t0 = time.time()
        log = os.path.join(work, p['name'] + '.out')
        with open(log, 'w') as f:
            rc = subprocess.run(cmd, env=env, stdout=f, stderr=subprocess.STDOUT, timeout=a.max_secs).returncode
        files = [n for n in os.listdir(out) if n.endswith('.fnrec')]
        size = sum(os.path.getsize(os.path.join(out, n)) for n in files)
        row = dict(p, rc=rc, seconds=round(time.time() - t0), files=len(files), bytes=size, out=out,
                   play=open(log).read().strip().splitlines()[-1][:300] if os.path.getsize(log) else '')
        if not files:
            shutil.rmtree(out, ignore_errors=True)
        with lock:
            results.append(row)
            with open(os.path.join(store, '%s-proofrun.jsonl' % a.game), 'a') as f:
                f.write(json.dumps(row) + '\n')
        print('%s: %d files, %.1f MB, %d s, rc %d' % (p['name'], len(files), size / 1e6, row['seconds'], rc), flush=True)

    def worker():
        while True:
            with lock:
                if not todo:
                    return
                p = todo.pop(0)
            try:
                one(p)
            except subprocess.TimeoutExpired:
                print('%s: over %d s' % (p['name'], a.max_secs), flush=True)

    threads = [threading.Thread(target=worker) for _ in range(max(1, min(2, a.jobs)))]
    for t in threads:
        t.start()
    for t in threads:
        t.join()


def run_dirs(game):
    scratch = os.environ.get('PLAYDM2' if game == 'dm2' else 'BMXCORPUS', '')
    return os.path.join(scratch, 'runs')


def cmd_tally(a):
    names = {t['addr']: t['name'] for t in read_targets(a.targets)}
    added = collections.defaultdict(lambda: collections.Counter())
    rows = [json.loads(l) for l in open(os.path.join(a.store, '%s-proofrun.jsonl' % a.game)) if l.strip()]
    line = re.compile(r'\[fncheck:newblocks\] address=0x([0-9a-f]+) name=\S+ vsync=\d+ added=(\d+)')
    for r in rows:
        log = os.path.join(run_dirs(a.game), r['name'], 'run.log')
        if not os.path.exists(log):
            continue
        for m in line.finditer(open(log, errors='replace').read()):
            added[int(m.group(1), 16)][r['name']] += int(m.group(2))
    total = sum(sum(c.values()) for c in added.values())
    print('%d sessions, %d kept; %d targets got new blocks, %d blocks in all' % (
        len(rows), sum(1 for r in rows if r['files']), len(added), total))
    for addr, c in sorted(added.items(), key=lambda kv: -sum(kv[1].values()))[:40]:
        print('  0x%08x %-36s +%d blocks (%s)' % (addr, names.get(addr, '?'), sum(c.values()),
                                               ', '.join('%s %d' % kv for kv in c.most_common(3))))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    t = sub.add_parser('targets')
    t.add_argument('game', choices=('dm2', 'bmx'))
    t.add_argument('depth')
    t.add_argument('out')
    t.add_argument('--blocks')
    t.add_argument('--extra', action='append')
    t.add_argument('--thin', action='store_true')
    p = sub.add_parser('plan')
    p.add_argument('game', choices=('dm2', 'bmx'))
    p.add_argument('targets')
    p.add_argument('out')
    p.add_argument('--seeds', type=int, default=2)
    p.add_argument('--ticks', type=int, default=6000)
    p.add_argument('--top', type=int, default=8)
    r = sub.add_parser('run')
    r.add_argument('game', choices=('dm2', 'bmx'))
    r.add_argument('plan')
    r.add_argument('store')
    r.add_argument('--targets', required=True)
    r.add_argument('--seed-file', required=True)
    r.add_argument('--jobs', type=int, default=2)
    r.add_argument('--limit', type=int, default=0)
    r.add_argument('--max-secs', type=int, default=3600)
    y = sub.add_parser('tally')
    y.add_argument('game', choices=('dm2', 'bmx'))
    y.add_argument('store')
    y.add_argument('targets')
    a = ap.parse_args()
    {'targets': cmd_targets, 'plan': cmd_plan, 'run': cmd_run, 'tally': cmd_tally}[a.cmd](a)


if __name__ == '__main__':
    main()
