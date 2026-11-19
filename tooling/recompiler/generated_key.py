#!/usr/bin/env python3
"""generated_key.py [--tree TREE] CONFIG...: the key of the generated code this tree's recompiler makes from the games'
CONFIGs (each a config.game.toml or config.ns.toml, every game's: one key for the set, as rcheck --gen-set takes one
tag): a hash of everything the output depends on, the recompiler's sources (this
folder's ps2xRecomp, its pinned libraries in CMakeLists.txt), the runtime's call lists it reads (TREE's
runtime/include/ps2_call_list.h and ps2_runtime_calls.h), the config (its paths aside), the Ghidra function list and
the ELF it names. A game's generated folder is named <folder>-<key> (recomp/generated-<key>,
recomp/generated-ns-<key>, each game's in its own data folder): made once per key beside the others, never written again, so trees with different
recompilers keep building side by side (make-generated.sh makes one)."""
import argparse
import hashlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PATH_KEYS = ('input', 'output', 'ghidra_output')


def config_paths(text, base):
    found = {}
    for key in PATH_KEYS:
        m = re.search(r'^%s\s*=\s*"([^"]*)"' % key, text, re.M)
        if m:
            p = m.group(1)
            found[key] = p if os.path.isabs(p) else os.path.normpath(os.path.join(base, p))
    return found


def key(configs, tree):
    h = hashlib.sha256()

    def add(label, data):
        h.update(label.encode() + b'\0' + hashlib.sha256(data).digest())

    recomp = os.path.join(HERE, 'ps2xRecomp')
    for root, dirs, files in os.walk(recomp):
        dirs[:] = sorted(d for d in dirs if d != 'tools')
        for f in sorted(files):
            if f.endswith(('.cpp', '.h', '.hpp', '.txt')):
                p = os.path.join(root, f)
                add(os.path.relpath(p, HERE), open(p, 'rb').read())
    for f in ('ps2_call_list.h', 'ps2_runtime_calls.h'):
        add(f, open(os.path.join(tree, 'runtime', 'include', f), 'rb').read())
    for config in configs:
        text = open(config, encoding='utf-8').read()
        paths = config_paths(text, os.path.dirname(os.path.abspath(config)))
        add('config', re.sub(r'^(%s)\s*=.*$' % '|'.join(PATH_KEYS), '', text, flags=re.M).encode())
        for k in ('input', 'ghidra_output'):
            if k in paths and os.path.exists(paths[k]):
                add(k, open(paths[k], 'rb').read())
            else:
                sys.exit('generated_key.py: %s names no %s (%s)' % (config, k, paths.get(k)))
    return h.hexdigest()[:12]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--tree', default=os.path.dirname(os.path.dirname(HERE)),
                    help='the z-axis tree whose runtime the recompiler reads (default: the one this tooling is in)')
    ap.add_argument('configs', nargs='+')
    a = ap.parse_args()
    print(key(a.configs, a.tree))


if __name__ == '__main__':
    main()
