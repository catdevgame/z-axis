#!/usr/bin/env python3
"""scaffolding.py: how much temporary scaffolding the readable source still has, per kind and folder.

    scaffolding.py [--json] [--root DIR]

The kinds and their patterns are docs/SCAFFOLDING.md's: each `## NAME` section with a `- find: `PATTERN`` line is
counted over runtime/src/game (.cpp and .h; not ee/, the framework that defines them), by top two folders
(dm2/rider, zge/scene, ...). A section without a find line is listed as counted by hand. The counts only inform: what each form is and what removes it is the file's.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import ztree  # noqa: E402  (the z-axis tree checked: ztree.py)
REPO = ztree.TREE
DOC = os.path.join(ztree.DOCS, 'SCAFFOLDING.md')
SOURCES = ztree.GAME


def kinds(doc):
    """[(name, pattern or None)] in the file's order."""
    out, name = [], None
    for line in open(doc, encoding='utf-8'):
        heading = re.match(r'^## (\S+)', line)
        if heading:
            name = heading.group(1)
            out.append([name, None])
            continue
        find = re.match(r'^- find: `(.+)`\s*$', line)
        if find and name:
            out[-1][1] = find.group(1)
    return [tuple(k) for k in out]


def folder(path):
    parts = os.path.relpath(path, SOURCES).split(os.sep)
    return '/'.join(parts[:2]) if len(parts) > 2 else parts[0]


def count(root, patterns):
    totals = {name: {} for name, pattern in patterns if pattern}
    compiled = [(name, re.compile(pattern)) for name, pattern in patterns if pattern]
    for directory, dirs, files in os.walk(root):
        if os.path.relpath(directory, root) == '.':
            dirs[:] = [d for d in dirs if d != 'ee']  # the framework defines the forms; its own uses are not scaffolding
        for f in files:
            if not f.endswith(('.cpp', '.h')):
                continue
            path = os.path.join(directory, f)
            text = open(path, encoding='utf-8', errors='replace').read()
            text = re.sub(r'//[^\n]*', '', text)  # a comment naming a form is not a use of it
            for name, rx in compiled:
                n = len(rx.findall(text))
                if n:
                    by = totals[name]
                    by[folder(path)] = by.get(folder(path), 0) + n
    return totals


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--json', action='store_true')
    ap.add_argument('--root', default=SOURCES)
    a = ap.parse_args()
    patterns = kinds(DOC)
    totals = count(a.root, patterns)
    if a.json:
        json.dump({name: totals.get(name) for name, _ in patterns}, sys.stdout, indent=1, sort_keys=True)
        print()
        return 0
    for name, pattern in patterns:
        if not pattern:
            print('%-24s counted by hand (docs/SCAFFOLDING.md)' % name)
            continue
        by = totals[name]
        top = ', '.join('%s %d' % (f, n) for f, n in sorted(by.items(), key=lambda x: -x[1])[:5])
        print('%-24s %6d  %s' % (name, sum(by.values()), top))
    return 0


if __name__ == '__main__':
    sys.exit(main())
