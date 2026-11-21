#!/usr/bin/env python3
"""readable_saves.py RUN_DIR [RUN_DIR ...] --out DIR [--tree SRC] [--generated DIR] [--no-drafts]
                 [--only ADDR,...|@FILE] [--held]

The hand-written functions that give a callee another value of a
callee-saved register than the original held there (zx-fncheck's
saves_differ: a register the callee's prologue saves, so in the game it
writes another value to its stack frame; drafter23's check, drafter24's
sites), one list per source file, for the hands agents.

From the corpus runs' result lines (fncheck_corpus.py run with a checker
that prints saves_sites, drafter24 on): per function, every call site of
the original (the jal's address) and callee where a register differed, the
registers, the files (recordings) and calls. Each site gets the line the
drafter writes there (readable_draft.py --saves-table: ee::calleeSaves with
the register numbers and the draft's values; the hand-written code puts the
same registers, with its own values for them, just before its call of that
callee). The original's value of the first difference is in first_saves.

DIR/index.txt lists the files, DIR/<path with / as __>.txt one file's
functions. --held also lists the registers the callee does not save itself
(held_sites: a deeper callee may), apart.

    readable_saves.py $ZAXIS_SCRATCH/<task>/runs/<run> --out $ZAXIS_SCRATCH/<task>/saves-by-file
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
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import readable_digest as rd  # noqa: E402

FIELD = re.compile(r'(\w+)=("(?:[^"]*)"|\S+)')
SITE = re.compile(r'0x([0-9a-f]+)@0x([0-9a-f]+):(\S+)')


def lines_of(d):
    for line in open(os.path.join(d, 'lines.jsonl')):
        f = json.loads(line)
        if f.get('kind') == 'result' and f.get('readable') == '1':
            yield f


def registrations(tree):
    """{(game, addr): (name, file relative to tree, status)} of the hand-written registrations."""
    out = {}
    for path in rd.sources([tree]):     # (the tree's readable code)
        for r in rd.file_digests(path):
            out[(r['game'], r['addr'])] = (r['name'], os.path.relpath(path, tree), r['status'])
    return out


def callee_names(tree, regs):
    names = {addr: name.split('::')[-1] for (game, addr), (name, _, _) in regs.items() if game == 'ai'}
    tsv = ztree.zlayout.path(tree, 'tools', 'readable', 'readable_names.tsv')
    if os.path.exists(tsv):
        for line in open(tsv):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 3 and p[0] == 'function' and re.match(r'^0x[0-9A-Fa-f]+$', p[1]):
                names.setdefault(int(p[1], 16), p[2])
    import glob
    for m in [p for _, d in ztree.zlayout.readable_dirs(tree) for p in glob.glob(os.path.join(d, 'sweep', 'manifest.txt'))]:
        for line in open(m):
            p = line.rstrip('\n').split('\t')
            if len(p) >= 7 and re.match(r'^0x[0-9A-Fa-f]+$', p[0]):
                names.setdefault(int(p[0], 16), p[6])
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--out', required=True)
    ap.add_argument('--tree', default=ztree.TREE, help='the z-axis tree (default: ztree.py\'s)')
    ap.add_argument('--generated', help='the recompiled sources (readable_draft.py --generated)')
    ap.add_argument('--no-drafts', action='store_true', help='no readable_draft.py run (no suggested lines)')
    ap.add_argument('--only')
    ap.add_argument('--held', action='store_true')
    a = ap.parse_args()
    regs = registrations(a.tree)
    only = None
    if a.only:
        words = []
        for item in a.only.split(','):
            words += [l.split()[0] for l in open(os.path.expanduser(item[1:]))] if item.startswith('@') else [item]
        only = set(int(w.rpartition(':')[2], 16) for w in words if w)
    per = {}
    for d in a.dirs:
        for f in lines_of(d):
            game = f.get('game', 'ai')
            addr = int(f['address'], 16)
            if (game, addr) not in regs or (only is not None and addr not in only):
                continue
            e = per.setdefault((game, addr), {'recorded': 0, 'saves': 0, 'held': 0, 'files': 0, 'first': None,
                                              'sites': collections.defaultdict(lambda: [set(), 0]),
                                              'held_sites': collections.defaultdict(lambda: [set(), 0])})
            e['recorded'] += int(f.get('recorded', 0))
            e['saves'] += int(f.get('saves_differ', 0))
            e['held'] += int(f.get('held_differ', 0))
            if int(f.get('saves_differ', 0)):
                e['files'] += 1
                e['first'] = e['first'] or f.get('first_saves', '').strip('"')
            for field, key in (('saves_sites', 'sites'), ('held_sites', 'held_sites')):
                for callee, site, rs in SITE.findall(f.get(field, '').strip('"')):
                    s = e[key][(int(callee, 16), int(site, 16))]
                    s[0] |= set(rs.split('+'))
                    s[1] += 1
    todo = sorted(k for k, e in per.items() if e['saves'] or (a.held and e['held']))
    lines_at = {}
    if todo and not a.no_drafts:
        with tempfile.TemporaryDirectory() as tmp:
            table = os.path.join(tmp, 'saves.txt')
            cmd = [sys.executable, '-B', os.path.join(ztree.TOOLS, 'readable_draft.py'), '--no-workdb', '-o',
                   os.path.join(tmp, 'drafts.cpp'), '--saves-table', table] + ['0x%x' % k[1] for k in todo if k[0] == 'ai']
            if a.generated:
                cmd += ['--generated', a.generated]
            p = subprocess.run(cmd, capture_output=True, text=True)
            if p.returncode != 0 or not os.path.exists(table):
                sys.stderr.write('readable_saves.py: readable_draft.py failed (%s); lists without the drafts\' lines\n'
                                 % (p.stderr.strip().splitlines() or ['?'])[-1])
            else:
                for line in open(table):
                    fn, site, callee, text = line.rstrip('\n').split(' ', 3)
                    lines_at[int(site, 16)] = text
    names = callee_names(a.tree, regs)
    by_file = collections.defaultdict(list)
    for k in todo:
        by_file[regs[k][1]].append(k)
    os.makedirs(a.out, exist_ok=True)
    index = []
    for path in sorted(by_file):
        out = ['# %s: hand-written functions whose calls give a callee another value of a callee-saved register '
               'than the original held there (zx-fncheck saves_differ; readable_saves.py)' % path,
               '# Per call: the original\'s jal address, the callee, the registers the callee saves (its prologue: '
               'its stack frame gets the value) and the drafter\'s line there (its values are the draft\'s names: '
               'publish the hand-written code\'s own values of those registers before that call).', '']
        calls = 0
        for k in sorted(by_file[path], key=lambda k: -per[k]['saves']):
            name, _, status = regs[k]
            e = per[k]
            calls += e['saves']
            out.append('%s 0x%06X %s (%s): %d of %d recorded calls, in %d recordings; first: %s' % (
                k[0], k[1], name, status, e['saves'], e['recorded'], e['files'], e['first'] or '-'))
            for (callee, site), (rs, n) in sorted(e['sites'].items(), key=lambda x: x[0][1]):
                out.append('  at 0x%06X call 0x%06X %s: %s (%d recordings)' % (
                    site, callee, names.get(callee, ''), '+'.join(sorted(rs, key=reg_order)), n))
                if site in lines_at:
                    out.append('      draft: %s;' % lines_at[site])
            if a.held:
                for (callee, site), (rs, n) in sorted(e['held_sites'].items(), key=lambda x: x[0][1]):
                    out.append('  held at 0x%06X call 0x%06X %s: %s (%d recordings)' % (
                        site, callee, names.get(callee, ''), '+'.join(sorted(rs, key=reg_order)), n))
            out.append('')
        fname = path.replace('/', '__') + '.txt'
        open(os.path.join(a.out, fname), 'w').write('\n'.join(out))
        index.append('%-70s %3d functions %8d calls  %s' % (path, len(by_file[path]), calls, fname))
    open(os.path.join(a.out, 'index.txt'), 'w').write(
        '# readable_saves.py over %s: %d hand-written functions, %d files\n' % (' '.join(a.dirs), len(todo), len(by_file)) +
        '\n'.join(index) + '\n')
    print('%d hand-written functions with saves_differ in %d files -> %s' % (len(todo), len(by_file), a.out))


def reg_order(r):
    m = re.match(r'([a-z]+)(\d*)', r)
    return ({'s': 0, 'fp': 1, 'ra': 2, 'f': 3}.get(m.group(1), 9), int(m.group(2) or 0))


if __name__ == '__main__':
    main()
