#!/usr/bin/env python3
"""readable_signatures.py --check [--tree SRC] [--out FILE] [--only ADDR,...]: every hand-written
readable function's signature (its integer and float parameters) against the argument registers
its original reads, as the drafter decides them (readable_draft.py's Callees, the readable
signature set aside): the registers its code reads before setting them, and those it hands on
to a callee that reads them, through a tail call too (drafter25, from zgehand10's sigcheck.py).

A readable caller calls a readable function directly (game::call) with only the arguments its
signature names; one its original hands on that the signature leaves out reaches the callee as
whatever the context holds (zgehand10: freeLoadedFile's signature had no parameter, the original
hands its a0 on to the heap's free with `j`; loadSkaterDynamics' game::call then freed another
block, a level-load hang that only logiccheck caught).

Two lists:
  fewer    the original reads (or hands on to a callee that reads) a register beyond the
           signature: exit status 1;
  handed   it hands registers beyond the signature on unchanged to an indirect call or a
           kernel call (a variadic callee: what it reads is not known statically). Listed
           with the registers; not counted in the exit status unless --strict. Write the
           parameter when the callee reads it, or mark the registration's line with
           `// handed on: <why none is read>` to take it off the list.

readable_signatures.py ADDR...: the same for some functions, hand-written or not.
"""
import argparse
import os
import re
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
import zaxis_paths  # noqa: E402  (tools/paths.env: the workspaces, the data, the work database)
import readable_symbols

HERE = os.path.dirname(os.path.abspath(__file__))
sys.dont_write_bytecode = True
sys.setrecursionlimit(20000)


def analyse(ws, cal, address):
    """(ints, floats, variadic, handed): the original's reads, the readable signature set aside;
    handed: the integer and float argument registers it hands on unchanged from its entry to an
    indirect or variadic callee."""
    import readable_draft as rd
    r = ws.readable.get(address)
    saved = dict(r) if r else None
    if r:
        r['signature'] = None
    cal.memo.pop(address, None)
    try:
        ints, floats, variadic = cal.reads(address)
        handed = set()
        if variadic:
            L = rd.Lifter(ws, address, 1)
            rd.function_params(L, cal, 1)
            for k in L.block_keys():
                b = L.blocks[k]
                sites = [(o.info, i) for i, o in enumerate(b.ops) if o.k == 'call']
                if b.term[0] == 'tail':
                    sites.append((b.term[1], len(b.ops)))
                for info, i in sites:
                    if info.target is not None and info.target == L.address:
                        continue
                    if info.target is not None and not cal.reads(info.target, 1)[2]:
                        continue
                    st = L.state_before(k, i)
                    for reg in rd.INT_ARGS + rd.FLOAT_ARGS:
                        if ('entry', reg) in st.get(reg, {('entry', reg)}):
                            handed.add(reg)
        return ints, floats, variadic, handed
    finally:
        if r:
            r.update(saved)
        cal.memo.pop(address, None)


def counts(signature):
    import readable_draft as rd
    params = signature[1] if signature else []
    return (sum(1 for t in params if not rd.is_float_type(t)), sum(1 for t in params if rd.is_float_type(t)))


def report_one(ws, cal, address, name):
    import readable_draft as rd
    r = ws.readable.get(address)
    ni, nf = counts(r['signature']) if r and r.get('signature') else (None, None)
    try:
        ints, floats, variadic, handed = analyse(ws, cal, address)
    except Exception as e:  # (a function the drafter cannot lift)
        return None, '0x%06X %s: not analysed (%s)' % (address, name, e)
    if ni is None:
        return None, '0x%06X %s: reads ints %s floats %s%s%s' % (
            address, name, ints, floats, ' variadic' if variadic else '',
            (', hands on ' + ' '.join(sorted(handed))) if handed else '')
    fewer_i = [rd.INT_ARGS[i] for i in ints if i >= ni]
    fewer_f = [rd.FLOAT_ARGS[i] for i in floats if i >= nf]
    beyond = sorted(x for x in handed if (x in rd.INT_ARGS and rd.INT_ARGS.index(x) >= ni) or
                    (x in rd.FLOAT_ARGS and rd.FLOAT_ARGS.index(x) >= nf))
    if fewer_i or fewer_f:
        return 'fewer', '0x%06X %s: the signature takes %d int, %d float; the original reads %s' % (
            address, name, ni, nf, ' '.join(fewer_i + fewer_f))
    if beyond:
        return 'handed', '0x%06X %s: the signature takes %d int, %d float; hands on to a variadic callee %s' % (
            address, name, ni, nf, ' '.join(beyond))
    return None, None


def hand_written(tree):
    """[(address, name, status, path, marked)] of the AI registrations under src/game (not the sweep's)."""
    import readable_digest as rd
    out = []
    for path in rd.sources([tree]):     # (the tree's readable code)
        lines = readable_symbols.literal_view(open(path, encoding='utf-8', errors='replace').read()).split('\n')
        regs = [r for r in rd.file_digests(path) if r['game'] == 'ai' and r.get('status') != 'Planned']
        for r in regs:
            marked = any(re.search(r'READABLE_FUNCTION\w*\(\s*0x0*%X\b' % r['addr'], l, re.I) and
                         '// handed on:' in l for l in lines)
            out.append((r['addr'], r['name'], r.get('status'), os.path.relpath(path, tree), marked))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--tree', default=ztree.TREE)
    ap.add_argument('--out')
    ap.add_argument('--only', help='comma-separated addresses (with --check)')
    ap.add_argument('--strict', action='store_true', help='count the handed list in the exit status')
    ap.add_argument('functions', nargs='*')
    a = ap.parse_args()
    sys.path.insert(0, HERE)
    import readable_draft as rd
    workspace = zaxis_paths.get('AI_WORKSPACE')
    generated = os.environ.get('ZX_RUNNER_GENERATED_DIR') or os.path.join(workspace, 'recomp', 'generated')
    ws = rd.Workspace(workspace, generated, None)
    cal = rd.Callees(ws)
    if not a.check:
        for f in a.functions:
            address = int(f, 16)
            kind, text = report_one(ws, cal, address, (ws.readable.get(address) or {}).get('name', f))
            print(text or '0x%06X: the signature covers what the original reads' % address)
        return 0
    only = set(int(x, 16) for x in a.only.split(',')) if a.only else None
    fewer, handed = [], []
    for address, name, status, path, marked in hand_written(a.tree):
        if only is not None and address not in only:
            continue
        kind, text = report_one(ws, cal, address, name)
        if kind == 'fewer':
            fewer.append('%s (%s, %s)' % (text, status, path))
        elif kind == 'handed' and not marked:
            handed.append('%s (%s, %s)' % (text, status, path))
    out = ['# readable_signatures.py --check: hand-written signatures that take fewer argument registers '
           'than the original reads (fewer), or that leave out registers it hands on to a variadic '
           'callee (handed)']
    out += ['fewer: %d' % len(fewer)] + ['  ' + l for l in fewer]
    out += ['handed: %d' % len(handed)] + ['  ' + l for l in handed]
    text = '\n'.join(out) + '\n'
    if a.out:
        open(a.out, 'w').write(text)
    sys.stdout.write(text)
    return 1 if fewer or (a.strict and handed) else 0


if __name__ == '__main__':
    sys.exit(main())
