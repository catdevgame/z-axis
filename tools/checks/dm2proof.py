#!/usr/bin/env python3
"""What a semantic replay of a game's corpus proves, by zbind level (docs/READABLE.md "Symbols").

    dm2proof.py REPLAY_DIR --levels BUILD/symbols/<game>/levels.tsv [--game dm2|bmx] [--subsystems TSV]
                                     [--out TSV] [--tested-out FILE] [--fail-list N]

REPLAY_DIR holds fncheck_corpus.py's lines.jsonl. A function's verdict over all its recordings:
  pass     every call of every file: the original gives the recording back and the readable body
           equals it (a call passes on what it does, `--semantic`)
  fail     a call differs, or the file faulted; `first` tells the first differing call
  original the traced original does not give the recording back: the recording cannot judge
  none     no recording has a call of it (no route reached it)
Level 2's bodies are the ones meant to run; level 1's are AI's C++ over code that differs here, built
for this replay only (--try-close): a pass says the shared body does what DM2's code does on the recorded
calls, a fail is a version difference until read otherwise.
"""
import argparse
import collections
import json
import os
import re


def lines(path):
    with open(path) as f:
        for text in f:
            try:
                yield json.loads(text)
            except ValueError:
                pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('replay')
    ap.add_argument('--game', default='dm2')
    ap.add_argument('--levels', required=True)
    ap.add_argument('--subsystems')
    ap.add_argument('--out')
    ap.add_argument('--tested-out')
    ap.add_argument('--fail-list', type=int, default=20)
    a = ap.parse_args()
    levels = {}
    for row in open(a.levels):
        p = row.rstrip('\n').split('\t')
        if len(p) >= 3:
            levels[int(p[1], 16)] = (p[0], int(p[2]), p[3] if len(p) > 3 else '')
    subsystem = {}
    if a.subsystems:
        for row in open(a.subsystems):
            p = row.rstrip('\n').split('\t')
            subsystem[int(p[0], 16)] = p[3]
    fn = collections.defaultdict(lambda: collections.Counter())
    first = {}
    for r in lines(os.path.join(a.replay, 'lines.jsonl')):
        if r.get('kind') == 'fault':     # (a fault's line names no game: its file does)
            m = re.search(a.game + r'-([0-9a-f]{8})\.fnrec', str(r.get('file', '')))
            if not m:
                continue
            r['address'], r['game'] = '0x' + m.group(1), a.game
        if r.get('game') != a.game or 'address' not in r:
            continue
        addr = int(r['address'], 16)
        c = fn[addr]
        if r.get('kind') == 'fault':
            c['fault' if 'readable' in str(r.get('phase', '')) or 'exit' in str(r.get('phase', '')) else 'fault_original'] += 1
            first.setdefault(addr, 'fault ' + str(r.get('phase', ''))[:60])
        elif r.get('kind') == 'result':
            c['recorded'] += int(r.get('recorded', 0))
            c['original_ok'] += int(r.get('original_ok', 0))
            c['original_bad'] += int(r.get('original_bad', 0))
            c['readable_ok'] += int(r.get('readable_ok', 0))
            c['readable_bad'] += int(r.get('readable_bad', 0))
            for k in ('first_readable', 'first_args_left', 'first_vu0_left', 'first_entry_left'):
                if r.get(k):
                    first.setdefault(addr, '%s %s' % (k, str(r[k])[:80]))

    def verdict(address):
        c = fn.get(address)
        if c and c['fault']:
            return 'fault'
        if c and c['fault_original']:
            return 'original'
        if not c or not c['recorded']:
            return 'none'
        if c['original_bad']:
            return 'original'
        if c['readable_bad']:
            return 'fail'
        return 'pass' if c['readable_ok'] else 'none'

    by = collections.defaultdict(collections.Counter)
    sub = collections.defaultdict(collections.Counter)
    calls = collections.Counter()
    rows = []
    for address, (name, level, why) in sorted(levels.items()):
        if level == 0:
            by[0]['no counterpart'] += 1
            continue
        v = verdict(address)
        by[level][v] += 1
        sub[(level, subsystem.get(address, '?'))][v] += 1
        if v == 'pass':
            calls[level] += fn[address]['readable_ok']
        rows.append((level, name, address, v, fn[address]['recorded'], first.get(address, ''), why))
    for level in sorted(by, reverse=True):
        print('level %d: %s' % (level, ', '.join('%s %d' % kv for kv in by[level].most_common())), '; calls passed', calls[level])
    for (level, s), c in sorted(sub.items(), key=lambda kv: (-kv[0][0], -sum(kv[1].values()))):
        print('  L%d %-22s %s' % (level, s, ' '.join('%s %d' % kv for kv in c.most_common())))
    fails = [r for r in rows if r[3] in ('fail', 'fault', 'original')]
    for level, name, address, v, n, why, reason in [r for r in fails if r[0] == 2][:a.fail_list]:
        print('  fail L%d %-30s 0x%06x %s (%d calls) %s' % (level, name[:30], address, v, n, why[:90]))
    if a.out:
        with open(a.out, 'w') as f:
            for level, name, address, v, n, why, reason in rows:
                f.write('%d\t%s\t0x%08x\t%s\t%d\t%s\t%s\n' % (level, name, address, v, n, why, reason))
    if a.tested_out:
        with open(a.tested_out, 'w') as f:
            for level, name, address, v, n, why, reason in rows:
                if level == 2 and v == 'pass':
                    f.write(name + '\n')


if __name__ == '__main__':
    main()
