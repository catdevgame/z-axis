#!/usr/bin/env python3
"""Coverage of a game's functions by the routes (PS2X_FNPROF profiles, or recording folders); --game dm2|bmx.

    dm2cov.py subsystems OUT.tsv               a subsystem per DM2 function, from the work database
    dm2cov.py report --subsystems TSV [--top N] DIR_OR_CSV ...
                                               DIRs of recordings or fnprof.csv files; the union, by
                                               subsystem, and what each input added

A function's subsystem is its AI counterpart's readable file (zge/assets, aggressive/level, ...),
else its rider map module (dm2/... or bmx/...), else its layer's neighbours by address (functions of one object file
sit together), else `<layer>:unknown`.
"""
import argparse
import bisect
import collections
import csv
import io
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def query(sql):
    p = subprocess.run([sys.executable, os.path.join(HERE, 'workdb.py'), 'query', sql, '--csv'], capture_output=True,
                       text=True, check=True)
    return list(csv.DictReader(io.StringIO(p.stdout)))


def subsystem_of_file(path):
    parts = path.split('/')
    i = parts.index('game') + 1 if 'game' in parts else 0
    parts = parts[i:]
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def subsystems(out, game):
    build = game + '_ps2'
    fns = query("select addr, end_addr, layer from functions where build='%s' order by addr" % build)
    pair = {int(r['addr']): r['file'] for r in query(
        "select c.addr, any_value(r.file) file from counterparts c join readable r on r.addr=c.ai_addr "
        "where c.build='%s' and c.confidence in ('high','medium') and r.file is not null group by 1" % build)}
    maps = {int(r['addr']): r['m'] for r in query(
        "select addr, any_value(module) m from func_map_latest where build='%s' group by 1" % build)}
    rows = []
    for r in fns:
        a = int(r['addr'])
        s = None
        if r['layer'] == 'sdk':
            s = 'sdk'
        elif a in maps and maps[a]:
            s = game + '/' + maps[a].split()[0].replace('.c', '')
        elif a in pair:
            s = subsystem_of_file(pair[a])
        rows.append([a, int(r['end_addr']), r['layer'], s])
    # Neighbours: the nearest classified function of the same layer within 6 places.
    for i, row in enumerate(rows):
        if row[3]:
            continue
        for d in range(1, 7):
            for j in (i - d, i + d):
                if 0 <= j < len(rows) and rows[j][3] and rows[j][2] == row[2] and rows[j][3] != 'sdk':
                    row[3] = rows[j][3]
                    break
            if row[3]:
                break
        row[3] = row[3] or row[2] + ':unknown'
    with open(out, 'w') as f:
        for a, e, layer, s in rows:
            f.write('0x%08x\t0x%08x\t%s\t%s\n' % (a, e, layer, s))
    print(len(rows), collections.Counter(r[3] for r in rows).most_common(40))


def read_subsystems(path):
    out = {}
    for line in open(path):
        a, e, layer, s = line.rstrip('\n').split('\t')
        out[int(a, 16)] = (int(e, 16), layer, s)
    return out


def functions_of(path):
    """{address: calls} of a profile or a recording folder."""
    if os.path.isdir(path):
        out = {}
        for f in os.listdir(path):
            m = re.match(r'(?:dm2-|bmx-)?([0-9a-f]{8})\.fnrec', f)
            if m and os.path.getsize(os.path.join(path, f)):
                out[int(m.group(1), 16)] = 1
        return out
    out = {}
    for r in csv.reader(open(path)):
        if len(r) >= 2 and r[0].startswith(('0x', '00')):
            calls = int(r[1])
            if calls:
                out[int(r[0], 16)] = calls
    return out


def report(args):
    table = read_subsystems(args.subsystems)
    ends = sorted(table)
    union, added = {}, []
    for path in args.inputs:
        got = functions_of(path)
        known = {a for a in got if a in table}
        new = known - set(union)
        union.update({a: got[a] for a in known})
        added.append((path, len(known), len(new)))
    for path, n, new in added:
        print('%-46s %5d functions, %4d new' % (os.path.basename(path.rstrip('/')) or path, n, new))
    all_by = collections.Counter(v[2] for v in table.values())
    hit_by = collections.Counter(table[a][2] for a in union)
    bytes_all = sum(v[0] - a for a, v in table.items())
    bytes_hit = sum(table[a][0] - a for a in union)
    print('total %d of %d functions (%.1f%%), %.1f%% of the code bytes' % (
        len(union), len(table), 100.0 * len(union) / len(table), 100.0 * bytes_hit / bytes_all))
    layers = collections.Counter(v[1] for v in table.values())
    hit_l = collections.Counter(table[a][1] for a in union)
    print(' '.join('%s %d/%d' % (k, hit_l[k], layers[k]) for k in layers))
    for s, n in all_by.most_common(args.top):
        print('  %-26s %4d / %4d  %5.1f%%' % (s, hit_by[s], n, 100.0 * hit_by[s] / n))
    if args.missing:
        with open(args.missing, 'w') as f:
            for a in sorted(set(table) - set(union)):
                f.write('0x%08x\t%s\t%d\n' % (a, table[a][2], table[a][0] - a))


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('subsystems')
    p.add_argument('out')
    p.add_argument('--game', default='dm2')
    p = sub.add_parser('report')
    p.add_argument('--subsystems', required=True)
    p.add_argument('--top', type=int, default=40)
    p.add_argument('--missing')
    p.add_argument('inputs', nargs='+')
    a = ap.parse_args()
    if a.cmd == 'subsystems':
        subsystems(a.out, a.game)
    else:
        report(a)


if __name__ == '__main__':
    main()
