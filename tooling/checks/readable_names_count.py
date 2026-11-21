#!/usr/bin/env python3
"""Counts the machine-shaped symbols left in the readable C++ (docs/READABLE.md,
"Names"): in the sweep's generated drafts (the build dir's readable_sweep) and
in the hand-written sources (src/game, without the sweep and drafts folders).

    readable_names_count.py [--sweep DIR] [--src DIR] [--json] [--by-file]

Kinds (comments and string literals are left out; each is counted as uses and
as distinct symbols per file):

  functions        fn_XXXXXX / FUN_XXXXXXXX / sub_XXXXXX names (definitions,
                   registrations and calls)
  callee consts    kFunXXXXXX: a callee's address constant with no name
  globals          g_XXXXXX / DAT_XXXXXXXX: a fixed address with no name
  fields           field_0x.. / unk.. names, and memory read at a raw offset
                   from a variable (ee::at<T>(p + 0x10)): a field with no name
  fields, indirect the same, through a pointer read in place, an io access or a
                   quadword's aligned address
  params           param_N / argN / fargN (Ghidra's or the drafter's)
  locals           register-named variables (v0, a1_2, s0, f12_3, ...), Ghidra's
                   iVar1 / uVar2 / local_10 / auStack_20
  numbered consts  other constants named by a number (kSomething_1C2D30,
                   kConst12)
  register reads   (not machine-shaped: counted apart) the values the original
                   reads from a register it never sets, named for it
                   (leftT0, callerS4: ee::leftByCall, ee::callerRegister)
  labels           goto labels named by an address (at_104728)
  page constants   a page's high half with no name but its address (kHigh280000)

The coverage page can call it (`--json`) for the before/after of a naming
round; it reads only the files, so it runs anywhere.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
GAME_SRC = ztree.GAME

REG = (r'(?:v[01]|a[0-7]|t[0-9]|s[0-8]|at|k[01]|gp|fp|ra|f(?:[0-9]|[12][0-9]|3[01])|product\d*|taken|'
       r'lo\d|hi\d|high\d+|acc\d+)')
KINDS = [
    ('functions', re.compile(r'\b(?:fn|FUN|sub|thunk_FUN)_[0-9A-Fa-f]{5,8}(?:_[0-9A-Fa-f]{6})?\b')),
    ('callee consts', re.compile(r'\bkFun[0-9A-F]{6}\d*\b')),
    ('globals', re.compile(r'\b(?:g_[0-9A-F]{6}(?:_[a-z0-9]+)?|DAT_[0-9a-fA-F]{8})\b')),
    ('fields', re.compile(r'\b(?:field_(?:0x)?[0-9A-Fa-f]+|m?_?unk(?:nown)?_?(?:0x)?[0-9A-Fa-f]+\w*)\b'
                          r'|ee::at<[^<>]+>\(\s*[A-Za-z_][\w.]*\s*\+\s*(?:0x[0-9A-Fa-f]+u?|\d+u?)\s*\)')),
    # (names8) the same raw offsets written so that `fields` does not see them: through
    # a pointer read in place (*ee::at<int32_t>(a + 4) + 8, *g_x + 8), the io access of an
    # address the drafter cannot prove is RAM, a quadword's aligned address.
    ('fields, indirect', re.compile(r'ee::io::(?:loadAny|storeAny)<[^<>]+>\(\s*[A-Za-z_][\w.]*\s*\+\s*(?:0x[0-9A-Fa-f]+u?|\d+u?)\s*[,)]'
                                    r'|ee::at<[^<>]+>\(\s*\*(?:ee::at<[^<>]+>\([^()]*\)|g_\w+)\s*\+\s*(?:0x[0-9A-Fa-f]+u?|\d+u?)\s*\)'
                                    r'|ee::at<[^<>]+>\(\s*\(\s*[A-Za-z_][\w.]*\s*\+\s*(?:0x[0-9A-Fa-f]+u?|\d+u?)\s*&\s*~0xFu\)')),
    ('params', re.compile(r'\b(?:param_\d+|f?arg\d+)\b')),
    ('locals', re.compile(r'(?<![\w.:>])(?:' + REG + r'(?:_\d+)?|' + REG + r'_hi(?:_\d+)?|[iuf]Var\d+|[bscp]Var\d+|'
                          r'local_[0-9a-f]+|auStack_[0-9a-f]+|uStack_[0-9a-f]+|stk_[0-9a-fA-F]+(?:_\d+)?)\b(?!\s*[(:<])')),
    ('numbered consts', re.compile(r'\bk(?!Fun[0-9A-F]{6})[A-Z]\w*?_[0-9A-F]{6}\b|\bkConst\d+\b')),
    # Not machine-shaped but named for a register by necessity: the values
    # the original reads from a register it never sets (ee::callerRegister,
    # ee::leftByCall), named leftT0 / callerS4 by readable_names.py.
    ('register reads', re.compile(r'(?<![\w.:>])(?:left|caller)(?:[VAST][0-9]|F[0-9]+|Gp|Fp|Ra|At|K[01])(?:High)?\d*\b')),
    # Named for an address: goto labels and the high half of a page.
    ('labels', re.compile(r'\bat_[0-9a-f]{5,8}(?:_likely)?\b')),
    ('page constants', re.compile(r'\bkHigh[0-9A-F]{6}\b')),
]


def strip_code(text):
    """The text with comments and string / char literals blanked (lines kept)."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i + 1] == '/':
            j = text.find('\n', i)
            j = n if j < 0 else j
            i = j
            continue
        if c == '/' and i + 1 < n and text[i + 1] == '*':
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j))
            i = j
            continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out.append(c + c)
            i = j + 1
            continue
        out.append(c)
        i += 1
    return ''.join(out)


def count_file(path):
    text = strip_code(open(path, encoding='utf-8', errors='replace').read())
    res = {}
    for kind, pat in KINDS:
        found = [m.group(0) for m in pat.finditer(text)]
        if kind.startswith('fields'):
            found = [re.sub(r'\s+', '', f) for f in found]
        res[kind] = (len(found), len(set(found)))
    return res


def walk(root, skip_generated):
    for dirpath, dirs, files in os.walk(root):
        dirs.sort()
        if skip_generated and os.path.basename(dirpath) in ('sweep', 'drafts'):
            dirs[:] = []
            continue
        for f in sorted(files):
            if f.endswith(('.cpp', '.h')):
                yield os.path.join(dirpath, f)


def current_files(root):
    """The batch files of the last draft (its spans.json), when the folder has
    one: a batch a sweep has removed stays in the build dir."""
    p = os.path.join(root, 'spans.json')
    if not os.path.exists(p):
        return None
    return {os.path.join(root, f) for f in json.load(open(p))}


def count_tree(root, skip_generated):
    total = {k: [0, 0] for k, _ in KINDS}
    files = {}
    nfiles = 0
    only = None if skip_generated else current_files(root)
    for path in walk(root, skip_generated):
        if only is not None and path not in only:
            continue
        nfiles += 1
        c = count_file(path)
        files[os.path.relpath(path, root)] = c
        for k, (uses, distinct) in c.items():
            total[k][0] += uses
            total[k][1] += distinct
    return {'files': nfiles, 'kinds': {k: {'uses': v[0], 'distinct': v[1]} for k, v in total.items()}}, files


def default_sweep():
    root = ztree.TREE
    out = os.path.join(root, 'out')
    best = None
    if os.path.isdir(out):
        for b in sorted(os.listdir(out)):
            p = os.path.join(out, b, 'runtime', 'readable_sweep')
            if os.path.isdir(p) and (best is None or os.path.getmtime(p) > os.path.getmtime(best)):
                best = p
    return best


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--sweep', help='the generated drafts (default: the newest out/*/runtime/readable_sweep)')
    ap.add_argument('--src', default=GAME_SRC, help='the hand-written sources (default %(default)s)')
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--by-file', action='store_true', help='also each file\'s counts')
    args = ap.parse_args()
    sweep = args.sweep or default_sweep()
    result = {}
    per_file = {}
    if sweep and os.path.isdir(sweep):
        result['drafts'], per_file['drafts'] = count_tree(sweep, False)
        result['drafts']['root'] = sweep
    result['hand-written'], per_file['hand-written'] = count_tree(args.src, True)
    result['hand-written']['root'] = args.src
    if args.by_file:
        result['by_file'] = per_file
    if args.json:
        json.dump(result, sys.stdout, indent=1, sort_keys=True)
        sys.stdout.write('\n')
        return
    for part, r in result.items():
        if part == 'by_file':
            continue
        print('%s (%d files, %s)' % (part, r['files'], r['root']))
        for k, _ in KINDS:
            v = r['kinds'][k]
            print('  %-16s %8d uses  %7d distinct' % (k, v['uses'], v['distinct']))


if __name__ == '__main__':
    main()
