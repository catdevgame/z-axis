#!/usr/bin/env python3
"""readable_marks.py --check BUILD [--tree SRC] [--status S,...] [--corpus DIR] [--all] [--out FILE]

Every readable function, hand-written (a registration in the readable code) or drafted
(BUILD's readable_sweep), that does not mark an entry block the original can run slice by slice.
Once a call or a loop of a function is interrupted the original may go on through the recompiler's
entry blocks, charging 8 and checking at each (ee/abi.h "safe points"); only ee::block,
ee::loopBack and ee::afterCall let the readable version follow, so a function without them is
exact only while a call runs whole (marks1, after quar1's __ieee754_rem_pio2f and drawHitFaces).

Owed: the entry blocks (fncheck_traced_gen.py's g_safePointMarks, the addresses the function table
maps to an entry_ function) reachable from a place the scheduler can resume a call of the function
at slice by slice: an entry block that is a call's return address or a branch's loop head (or the
return of a syscall), and every entry block of a function with a JR the recompiler made no switch
of (a dispatch even from the whole function). Everything else in the list runs only as part of the whole function, where
a mark does nothing (ee::block charges only in kBlocks; a branch of another function into this
one's code belongs to that function's call): --all lists those as well, apart. For the resume points
themselves the marker's kind is checked: a loop head needs ee::loopBack (ee::chargeLoop leaves
safe-point differences), a call's return ee::afterCall or ee::block(L, true).

This is the static side of zx-fncheck --markers (fncheck_corpus.py markers): it sees the paths no
recording ran, by the addresses the definition and its helpers (same file, headers) name; the replay proves
the order of the ones that ran. --corpus DIR adds the entry blocks that run's markers lines say
were reached unmarked and that are owed (a mark named here but never passed shows there).

Exit status 1 if anything owed is listed (--warn, the build's step after linking zx-fncheck: 0,
and only a line). --status limits the list (default: all statuses).

    readable_marks.py --check out/<build> --out $ZAXIS_SCRATCH/<task>/marks.txt
"""
import argparse
import collections
import json
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readable_symbols
import zaxis_paths  # noqa: E402  (the tree's readable folders)

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.path.insert(0, HERE)
import fncheck_traced_gen as gen  # noqa: E402
import readable_digest as rd  # noqa: E402

GAME_IDS = {0: 'ai', 1: 'dm2'}

# Entry blocks a resume can reach by the code's shape but no call can, for a reason the listing
# does not show. (game, function): {address: why}.
NEVER_RUN = {
    ('ai', 0x1AD550): {0x1AD57C: 'buildMirrorMap: the unaligned copy; the pairs table is 8-byte aligned'},
    ('ai', 0x142A88): {0x142BC0: 'createObjectAnimator: the inner loop\'s empty case; the outer loop runs only with channels'},
    ('ai', 0x1A8550): {0x1A8644: 'inlineMenuPopulateSkaterStats: the unaligned copy; the shot table and its stack copy are 8-byte aligned'},
    ('ai', 0x176BC0): {0x176DBC: 'defineGuiKeyboardProperties: the unaligned copy; the key table and its stack copy are 8-byte aligned'},
}


def marks_table(build):
    """{(game, function): {address: flags}}"""
    text = open(os.path.join(build, 'runtime', 'fncheck_build', 'fncheck_stack_args.cpp')).read()
    sec = text[text.index('g_safePointMarks[]'):text.index('g_safePointMarkCount')]
    out = {}
    for g, f, a, k in re.findall(r'\{(\d+)u, 0x([0-9a-f]+)u, 0x([0-9a-f]+)u, (\d+)u\}', sec):
        if int(f, 16) and int(k) & 1:  # (flags 4: a delay slot that ends an entry block, no mark)
            out.setdefault((GAME_IDS.get(int(g), g), int(f, 16)), {})[int(a, 16)] = int(k)
    return out


def generated_files(build):
    """{(game, function): the generated original}."""
    files = {}
    for line in open(os.path.join(build, 'runtime', 'fncheck_build', 'traced_blocks.list'), encoding='utf-8'):
        p = line.rstrip('\n').split('\t')
        if len(p) >= 3 and not p[0].startswith('#'):
            files[(GAME_IDS.get(int(p[0]), p[0]), int(p[1], 16))] = p[2]
    return files


def cases(text, jr):
    """The targets of the jump table the JR at `jr` takes, or None."""
    i = text.find('// 0x%x:' % jr)
    m = re.search(r'switch \(jumpTarget\) \{(.*?)\}', text[i:i + 40000], re.S) if i >= 0 else None
    if not m or 'dispatchGuestBranch' in text[i:i + m.start()]:
        return None  # (that switch is a later JR's)
    return [int(c, 16) for c in re.findall(r'case 0x([0-9A-Fa-f]+)u:', m.group(1))]


def analyse(path, entries):
    """(owed, loop heads, call returns) of a function, all within `entries`: what a resume can reach
    slice by slice, and the resume points by kind."""
    text = readable_symbols.literal_view(open(path, encoding='utf-8', errors='replace').read())
    insns = gen.instructions(text)
    m = gen.RANGE.search(text)
    start, end = int(m.group(1), 16), int(m.group(2), 16)
    heads = {a for a, k in entries.items() if k & 2}
    returns = set()
    resume = set(heads)
    for a, (w, _, _) in insns.items():
        kind, _ = gen.control(a, w)
        if kind == 'call' and a + 8 in entries:
            returns.add(a + 8)
        if w & 0xFC00003F == 0x0000000C and a + 4 in entries:  # syscall
            resume.add(a + 4)
        # A JR the recompiler made no switch of dispatches its target from the whole function too.
        if kind == 'jr' and (w >> 21) & 31 != 31 and w != 0x42000018 and cases(text, a) is None:
            resume |= set(entries)
    resume |= returns
    seen, todo = set(), list(resume)
    while todo:
        a = todo.pop()
        if a in seen or a not in insns or not start <= a < end:
            continue
        seen.add(a)
        w = insns[a][0]
        kind, target = gen.control(a, w)
        if kind == 'branch':
            todo.append(target)
            if not (w >> 26 == 4 and (w >> 16) & 0x3FF == 0):  # (b: beq zero, zero)
                todo.append(a + 8)
        elif kind == 'jump':
            todo.append(target)
        elif kind == 'call':
            todo.append(a + 8)
        elif kind == 'jr':
            if (w >> 21) & 31 != 31 and w != 0x42000018:
                table = cases(text, a)
                todo += entries if table is None else table
        else:
            todo.append(a + 4)
    return {a for a in entries if a in seen}, heads, returns


def named(body):
    """What the code names: addresses at all (ee::block's literal, a table of leaders), the ones
    given to ee::loopBack and to the markers that take a call's return, and whether any marker has
    a computed address (a helper shared by several functions, the address its parameter's or
    base + offset): those are left to the replay."""
    have = {int(x, 16) for x in re.findall(r'\b0x([0-9A-Fa-f]{5,8})u?\b', body)}
    loops = {int(x, 16) for x in re.findall(r'\bloopBack\s*\(\s*0x([0-9A-Fa-f]+)', body)}
    rets = {int(x, 16) for x in re.findall(r'\bafterCall\s*\(\s*0x([0-9A-Fa-f]+)', body)}
    rets |= {int(x, 16) for x in re.findall(r'\bblock\s*\(\s*0x([0-9A-Fa-f]+)u?\s*,\s*(?!false)', body)}
    rets |= {int(x, 16) for x in re.findall(r'\bselfCall\s*\(\s*0x([0-9A-Fa-f]+)', body)}
    rets |= {int(x, 16) for x in re.findall(r'\bjumpDispatch\s*\(\s*0x([0-9A-Fa-f]+)', body)}
    computed_block = bool(re.search(r'\bblock\s*\(\s*(?!0x)|\bblock\s*\(\s*0x[0-9A-Fa-f]+u?\s*[-+*]', body))
    computed_loop = bool(re.search(r'\bloopBack\s*\(\s*(?!0x)', body))
    computed_ret = bool(re.search(r'\b(?:afterCall\s*\(\s*(?!0x)|block\s*\(\s*(?!0x)[^,()]+,)', body))
    return have, loops, rets, computed_block, computed_loop, computed_ret


def missing(tree, build, marks, statuses):
    """[(path, game, addr, name, status, owed blocks, inert blocks, loop heads, call returns)]."""
    report = []
    files = generated_files(build)
    roots = [tree]     # (the readable code: zaxis_paths.walk of the tree)
    sweep = os.path.join(build, 'runtime', 'readable_sweep')
    if os.path.isdir(sweep):
        roots.append(sweep)
    shared = {}  # helpers defined in headers (the action tricks' shared bodies)
    for root, _, names in zaxis_paths.walk(roots[0]):
        for name in names:
            if name.endswith('.h'):
                text = readable_symbols.literal_view(open(os.path.join(root, name), encoding='utf-8', errors='replace').read())
                for fn, bodies in rd.definitions(rd.strip(text)).items():
                    shared.setdefault(fn, []).extend(bodies)
    for path in rd.sources(roots):
        regs = [r for r in rd.file_digests(path) if (r['game'], r['addr']) in marks and (r['game'], r['addr']) in files]
        if statuses:
            regs = [r for r in regs if r['status'].lower() in statuses]
        if not regs:
            continue
        code = rd.strip(readable_symbols.literal_view(open(path, encoding='utf-8', errors='replace').read()))
        defs = {**shared, **rd.definitions(code)}
        for r in regs:
            key = (r['game'], r['addr'])
            seen, todo, body = set(), [r['name'].split('::')[-1]], ''
            while todo:
                name = todo.pop()
                if name in seen or name not in defs:
                    continue
                seen.add(name)
                text = ' '.join(defs[name])
                body += ' ' + text
                todo += [i for i in set(rd.IDENT.findall(text)) if i in defs and i not in seen]
            entries = marks[key]
            owed, heads, returns = analyse(files[key], entries)
            owed -= set(NEVER_RUN.get(key, ()))
            have, loops, rets, computed_block, computed_loop, computed_ret = named(body)
            # (a marker with a computed address, a helper shared by several functions: the replay's)
            blocks = [] if computed_block else sorted(a for a in entries if a not in have and a in owed)
            inert = sorted(a for a in entries if a not in have and a not in owed)
            no_loop = [] if computed_loop else sorted(a for a in heads if a in have and a not in loops)
            no_ret = [] if computed_ret else sorted(a for a in returns if a in have and a not in rets)
            if blocks or inert or no_loop or no_ret:
                report.append((os.path.relpath(path, tree) if path.startswith(tree) else os.path.relpath(path, build),
                               r['game'], r['addr'], r['name'], r['status'], blocks, inert, no_loop, no_ret))
    return report


def corpus_rows(corpus):
    """{(game, addr): (blocks, loops)}: entry blocks some file reached and no file marked."""
    per = {}
    for line in open(os.path.join(corpus, 'lines.jsonl')):
        f = json.loads(line)
        if f.get('kind') != 'markers':
            continue
        e = per.setdefault((f.get('game', 'ai'), int(f['address'], 16)), [set(), set(), set(), set()])
        for i, col in enumerate(('unmarked_blocks', 'unmarked_loops', 'marked_blocks_at', 'marked_loops_at')):
            e[i] |= {int(x, 16) for x in (f.get(col) or '').split(',') if x and x != '-'}
    return {k: (sorted(e[0] - e[2]), sorted(e[1] - e[3])) for k, e in per.items() if (e[0] - e[2]) or (e[1] - e[3])}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--check', dest='build', required=True)
    ap.add_argument('--tree', default=os.path.dirname(os.path.dirname(HERE)))
    ap.add_argument('--status', help='comma-separated statuses (verified,tested,draft)')
    ap.add_argument('--corpus', help='a corpus run: its markers lines as well')
    ap.add_argument('--all', action='store_true', help='the entry blocks no resume can reach too')
    ap.add_argument('--out')
    ap.add_argument('--warn', action='store_true', help='for the build: exit status 0, one line when anything is owed')
    a = ap.parse_args()
    marks = marks_table(a.build)
    statuses = {s.strip().lower() for s in a.status.split(',')} if a.status else None
    rows = missing(a.tree, a.build, marks, statuses)
    out, by = [], collections.Counter()
    owed_fns = inert_total = 0
    nblocks = nloops = nrets = 0
    hexes = lambda xs: ','.join('0x%X' % x for x in xs)
    owed_keys = {}
    for path, game, addr, name, status, blocks, inert, no_loop, no_ret in sorted(rows):
        inert_total += len(inert)
        if blocks or no_loop or no_ret:
            owed_fns += 1
            by[status.lower()] += 1
            nblocks += len(blocks)
            nloops += len(no_loop)
            nrets += len(no_ret)
            owed_keys[(game, addr)] = set(blocks)
            parts = []
            if blocks:
                parts.append('%d entry blocks unmarked: %s' % (len(blocks), hexes(blocks)))
            if no_loop:
                parts.append('loop heads without ee::loopBack: ' + hexes(no_loop))
            if no_ret:
                parts.append('call returns without ee::afterCall or ee::block(L, true): ' + hexes(no_ret))
            out.append('%s %s:0x%06X %s (%s): %s' % (path, game, addr, name, status, '; '.join(parts)))
        if a.all and inert:
            out.append('%s %s:0x%06X %s (%s): no resume reaches: %s' % (path, game, addr, name, status, hexes(inert)))
    if a.corpus:
        for (game, addr), (blocks, loops) in sorted(corpus_rows(a.corpus).items()):
            owed = [b for b in blocks if b in owed_keys.get((game, addr), ())]
            if owed or loops:
                out.append('replay %s:0x%06X: reached, unmarked: %s; loops: %s' % (
                    game, addr, hexes(owed) or '-', hexes(loops) or '-'))
    summary = ('%d functions owe marks: %d entry blocks, %d loop heads without ee::loopBack, %d call returns without '
               'ee::afterCall (%s); %d entry blocks no resume reaches left unmarked' % (
                   owed_fns, nblocks, nloops, nrets, ', '.join('%s %d' % kv for kv in sorted(by.items())) or 'none',
                   inert_total))
    text = '\n'.join(out + [summary]) + '\n'
    if a.out:
        open(a.out, 'w').write('# readable_marks.py --check %s\n%s' % (a.build, text))
    if a.warn:
        if owed_fns:
            print('readable_marks: %s; readable_marks.py --check lists them' % summary)
        return 0
    sys.stdout.write(text)
    return 1 if owed_fns else 0


if __name__ == '__main__':
    sys.exit(main())
