"""Where the checks find the z-axis tree they check.

The checks live here in the tooling, the tree they read is a z-axis checkout (the main one or a lane's worktree).
The tree is, in this order:

  1. $ZAXIS_TREE (lanecheck.sh, merge-check.sh, batchS.sh and the remote jobs set it);
  2. the git worktree the current folder is in, when it is a z-axis tree (it has engine/zge);
  3. ZAXIS_MAIN (tools/paths.env, zpaths.py).

Importing this puts the tree's tools/readable on sys.path, after this folder: the build's own tools the checks
import (readable_symbols, readable_digest, readable_draft, readable_entries, readable_names, fncheck_traced_gen,
zaxis_paths) are the tree's, as the build runs them.

    import ztree
    ztree.TREE, ztree.TOOLS (TREE/tools/readable), ztree.CHECKS (here)
    ztree.ENGINE, ztree.PS2, ztree.GAMES, ztree.TESTS, ztree.IOP, ztree.DOCS (TREE/engine ...; ztree.folder('ps2')
    the name alone)
    ztree.readable_path('zge/render/draw.cpp'), ztree.readable_key(path), ztree.readable_files(('.cpp',)): the
    readable code by its keys (zlayout.py: it is in engine/, ps2/ and games/)
"""
import os
import subprocess
import sys

CHECKS = os.path.dirname(os.path.abspath(__file__))


def _load(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(CHECKS, name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


zlayout = sys.modules.get('zlayout') or _load('zlayout')   # (by its file: sys.path stays as the caller has it)
TOOLING = os.path.dirname(CHECKS)
if os.path.isfile(os.path.join(TOOLING, 'zpaths.py')):
    # (the tooling these checks are in is the one in use, unless the caller named one: the tree's zaxis_paths.py loads
    # the main checkout's zpaths.py, which reads ZAXIS_TOOLING's paths.env, so a tooling copy's check runs the copy's
    # workspaces' scripts, run-game-dm2.sh and the logic tools, never main's (tools34))
    os.environ.setdefault('ZAXIS_TOOLING', TOOLING)


def is_tree(path):
    return bool(path) and os.path.isdir(zlayout.path(path, 'engine', 'zge'))


def _git_top(path):
    try:
        return subprocess.run(['git', '-C', path, 'rev-parse', '--show-toplevel'], capture_output=True, text=True,
                              timeout=10).stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def _main_checkout():
    module = os.path.join(TOOLING, 'zpaths.py')
    if not os.path.isfile(module):      # (a worker's copy of the checks has no tooling around it)
        return os.environ.get('ZAXIS_MAIN')
    import importlib.util
    spec = importlib.util.spec_from_file_location('zpaths', module)
    zpaths = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(zpaths)
    return zpaths.PATHS.get('ZAXIS_MAIN')


def find():
    named = os.environ.get('ZAXIS_TREE')
    if named:
        if not is_tree(named):
            raise SystemExit('%s: ZAXIS_TREE=%s is not a z-axis tree (no engine/zge)' % (
                os.path.basename(sys.argv[0] or 'ztree'), named))
        return os.path.abspath(named)
    top = _git_top(os.getcwd())
    if is_tree(top):
        return top
    main = _main_checkout()
    if is_tree(main):
        return main
    raise SystemExit('%s: no z-axis tree: set ZAXIS_TREE, or run from a z-axis checkout' % (
        os.path.basename(sys.argv[0] or 'ztree')))


TREE = find()


def folder(kind):
    """The tree's name of a top-level folder (zlayout.py)."""
    return zlayout.folder(TREE, kind)


ENGINE = zlayout.path(TREE, 'engine')
PS2 = zlayout.path(TREE, 'ps2')
GAMES = zlayout.path(TREE, 'games')
TESTS = zlayout.path(TREE, 'tests')
IOP = zlayout.path(TREE, 'iop')
DOCS = zlayout.path(TREE, 'docs')
TOOLS = zlayout.path(TREE, 'tools', 'readable')


def readable_path(key):
    return zlayout.readable_path(TREE, key)


def readable_key(path):
    return zlayout.readable_key(TREE, path)


def readable_files(exts=None, top=None):
    return zlayout.readable_files(TREE, exts, top)


os.environ['ZAXIS_TREE'] = TREE     # (a check another check runs, from any folder, reads the same tree)
if TOOLS not in sys.path:
    # (after this folder, which the checks put first: a check's own module wins over a same-named file a tree from
    # before the move still has)
    sys.path.insert(sys.path.index(CHECKS) + 1 if CHECKS in sys.path else 0, TOOLS)


# The readable code of any tree (TREE above, or another one a check is given), as tools/readable/zaxis_paths.py has
# them for the tree's own tools: a check needs no more than this folder (zlayout.py).
def key_of(path, tree=None):
    """PATH's key (zge/render/draw.cpp) with this host's separator, as os.path.relpath gave it from the one folder the
    readable code was before layout1; outside it, the path from the tree."""
    tree = tree or TREE
    key = zlayout.readable_key(tree, path)
    return key.replace('/', os.sep) if key is not None else os.path.relpath(path, tree)


def readable(tree, *parts):
    """The file the key PARTS name (os.path.join's parts); a name of the old src/game's own files is games/'s."""
    key = '/'.join(p.replace(os.sep, '/').strip('/') for p in parts if p)
    path = zlayout.readable_path(tree, key)
    return path if path is not None else os.path.join(tree, 'games', *key.split('/'))


def is_readable(path, tree=None):
    return zlayout.readable_key(tree or TREE, path) is not None


def walk(root, *key):
    """os.walk of the readable code when ROOT is a tree (with KEY, the folder it names), else of the folder ROOT
    (with KEY, os.path.join(ROOT, *KEY)); dirs sorted."""
    if is_tree(root):
        k = '/'.join(p.replace(os.sep, '/').strip('/') for p in key if p)
        if not k or '/' not in k:
            yield from zlayout.readable_walk(root, k or None)
            return
        root = readable(root, k)
    elif key:
        root = os.path.join(root, *key)
    for d, dirs, files in os.walk(root):
        dirs.sort()
        yield d, dirs, files
