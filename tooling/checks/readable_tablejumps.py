#!/usr/bin/env python3
"""readable_tablejumps.py BUILD ADDR...: the ee::tableJump lines (ee/dispatch.h) a readable
version of each function needs: for every JR through a jump table the recompiler made a switch
(fncheck_traced_gen.py's g_tableJumps in BUILD/ps2xRuntime/fncheck_build/fncheck_stack_args.cpp),
the entry block it is in and each case outside that block (drafter23). Hand-written code writes
them before the case's ee::block; ps2xFnCheck --safepoints flags a function that leaves one out.

readable_tablejumps.py --check BUILD [--tree SRC] [--out FILE]: every hand-written readable
function (a registration under runtime/src/game; the sweep's drafts are the drafter's, which
writes them) whose original takes a jump table from an entry block to a case outside it, and
whose definition has no ee::tableJump naming that case: listed, with the lines it needs, and
exit status 1 (drafter24). --out writes the list (and every line those functions need) to FILE.
"""
import argparse
import bisect
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
sys.dont_write_bytecode = True


def tables(build):
    text = open(os.path.join(build, 'ps2xRuntime', 'fncheck_build', 'fncheck_stack_args.cpp')).read()
    marks, jumps = {}, {}
    sec = text[text.index('g_safePointMarks[]'):text.index('g_safePointMarkCount')]
    for g, f, a, k in re.findall(r'\{(\d+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, (\d+)u\}', sec):
        if g == '0' and int(k) & 1:  # (flags 4: a delay slot that ends an entry block)
            marks.setdefault(int(f, 16), []).append(int(a, 16))
    sec = text[text.index('g_tableJumps[]'):text.index('g_tableJumpCount')]
    for g, f, b in re.findall(r'\{(\d+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u\}', sec):
        if g == '0':
            jumps.setdefault(int(f, 16), []).append(int(b, 16))
    gen = None
    for line in open(os.path.join(build, 'CMakeCache.txt')):
        if line.startswith('PS2X_RUNNER_GENERATED_DIR:'):
            gen = line.split('=', 1)[1].strip()
    return marks, jumps, gen


def needed(fn, marks, jumps, gen):
    """[(case, entry block, case is an entry block, the JR)] and notes."""
    out, notes = [], []
    leaders = sorted(marks.get(fn, []))
    path = next((os.path.join(gen, n) for n in os.listdir(gen) if n.endswith('_0x%x.cpp' % fn) and not n.startswith('entry_')), None)
    src = open(path).read() if path else ''
    for b in sorted(jumps.get(fn, [])):
        # the JR after the block's start, and its switch's cases
        m = re.search(r'// 0x([0-9a-f]+): 0x[0-9a-f]+\s+jr\s+\$(?!ra)\w+.*?switch \(jumpTarget\) \{(.*?)\}', src[src.find('0x%x:' % b):], re.S)
        if not m:
            continue
        jr = int(m.group(1), 16)
        cases = [int(c, 16) for c in re.findall(r'case 0x([0-9A-Fa-f]+)u:', m.group(2))]
        i = bisect.bisect_right(leaders, jr) - 1
        if i < 0:
            notes.append('  JR 0x%X before the first entry block: whole function only' % jr)
            continue
        lo, hi = leaders[i], leaders[i + 1] if i + 1 < len(leaders) else 1 << 32
        for c in cases:
            if not lo <= c < hi:
                out.append((c, lo, c in leaders, jr))
    return out, notes


def line_of(c, lo, leader, jr):
    return 'ee::tableJump(0x%06X, 0x%06X, %s); // the JR at 0x%X' % (c, lo, 'true' if leader else 'false', jr)


def check(a):
    import readable_digest as rd
    marks, jumps, gen = tables(a.build)
    tree = a.tree
    missing_all, report = 0, []
    for path in rd.sources([ztree.zlayout.path(tree, 'runtime', 'src', 'game')]):
        raw = open(path, encoding='utf-8', errors='replace').read()
        regs = [r for r in rd.file_digests(path) if r['game'] == 'ai' and r['addr'] in jumps]
        if not regs:
            continue
        code = rd.strip(raw)
        defs = rd.definitions(code)
        for r in regs:
            need, notes = needed(r['addr'], marks, jumps, gen)
            if not need:
                continue
            short = r['name'].split('::')[-1]
            # Its definition and the same-file helpers it reaches (transitively).
            seen, todo, body = set(), [short], ''
            while todo:
                name = todo.pop()
                if name in seen or name not in defs:
                    continue
                seen.add(name)
                text = ' '.join(defs[name])
                body += ' ' + text
                todo += [i for i in set(rd.IDENT.findall(text)) if i in defs and i not in seen]
            # A literal case covers itself; a computed one (animateProps:
            # tableJump(wayLeader(way), 0x1C4DA8u, true)) every case from
            # the entry block it names.
            have, slices, tabled = set(), set(), False
            for first, second in re.findall(r'\btableJump\s*\(\s*([^,]+?)\s*,\s*([^,)]+?)\s*[,)]', body):
                lit = re.match(r'^0x([0-9A-Fa-f]+)u?$', first)
                if lit:
                    have.add(int(lit.group(1), 16))
                elif re.match(r'^0x[0-9A-Fa-f]+u?$', second):
                    slices.add(int(second.rstrip('uU'), 16))
                else:
                    tabled = True   # case and block from a helper's parameters (operateVariables' enterCase)
            if tabled:
                # the cases from a table: any case address the file lists counts
                have |= set(int(x, 16) for x in re.findall(r'\b0x([0-9A-Fa-f]{5,8})u?\b', code))
            miss = [n for n in need if n[0] not in have and n[1] not in slices]
            if not miss:
                continue
            missing_all += len(miss)
            report.append('%s 0x%06X %s (%s): %d of the %d cases it takes from an entry block have no ee::tableJump' % (
                os.path.relpath(path, tree), r['addr'], r['name'], r['status'], len(miss), len(need)))
            report += ['  ' + line_of(*n) for n in miss] + notes
    text = '\n'.join(report) + ('\n' if report else '')
    if a.out:
        open(a.out, 'w').write('# readable_tablejumps.py --check %s: hand-written functions without the ee::tableJump '
                               'lines their jump tables need (write each before the case\'s ee::block)\n' % a.build + text)
    sys.stdout.write(text)
    print('%d cases missing in %d hand-written functions' % (missing_all, sum(1 for l in report if not l.startswith(' '))))
    return 1 if missing_all else 0


def main():
    if len(sys.argv) > 1 and sys.argv[1] == '--check':
        ap = argparse.ArgumentParser()
        ap.add_argument('--check', dest='build', required=True)
        ap.add_argument('--tree', default=ztree.TREE)
        ap.add_argument('--out')
        sys.path.insert(0, HERE)
        sys.exit(check(ap.parse_args()))
    build, addrs = sys.argv[1], [int(a, 16) for a in sys.argv[2:]]
    marks, jumps, gen = tables(build)
    for fn in addrs:
        print('0x%08X' % fn)
        need, notes = needed(fn, marks, jumps, gen)
        for n in need:
            print('  ' + line_of(*n))
        for n in notes:
            print(n)


if __name__ == '__main__':
    main()
