"""Where the tools here find what is outside this repository: the game workspaces (tools and their
data), the work database's tool, the data folder. The paths are the tooling's (tools/paths.env,
read by tools/zpaths.py); a variable of the same name set in the environment wins. The tooling is
looked for in this checkout, then in its main checkout (a checkout from before the tooling was folded
in may have left its submodule empty). With no tooling, get() returns the environment's value or None,
so only a tool that needs the default (and was given no path) stops.
"""
import importlib.util
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TREE = REPO     # (the tree these tools are in: its readable code is what they read)
_paths = None


def _load_layout():
    import importlib.util as util
    spec = util.spec_from_file_location('zlayout', os.path.join(REPO, 'tools', 'checks', 'zlayout.py'))
    module = util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# The tree's layout (tools/checks/zlayout.py): the readable code's folders and keys
zlayout = sys.modules.get('zlayout') or _load_layout()


def _main_checkout():
    try:
        common = subprocess.run(['git', '-C', REPO, 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return common[:-len('/.git')] if common.endswith('/.git') else None


def _load():
    global _paths
    if _paths is None:
        _paths = {}
        for root in (REPO, _main_checkout()):
            module = os.path.join(root or '', 'tools', 'zpaths.py')
            if root and os.path.isfile(module):
                spec = importlib.util.spec_from_file_location('zpaths', module)
                zpaths = importlib.util.module_from_spec(spec)
                spec.loader.exec_module(zpaths)
                _paths = dict(zpaths.PATHS)
                break
    return _paths


def get(name, required=True):
    """The path NAME (paths.env: AI_WORKSPACE, DM2_WORKSPACE, WORKDB_PY, ZAXIS_MAIN, ...)."""
    value = os.environ.get(name) or _load().get(name)
    if value is None and required:
        raise SystemExit('%s: no %s: set it (tools/paths.env)' % (
            os.path.basename(sys.argv[0] or 'zaxis_paths'), name))
    return value


def key_of(path, tree=TREE):
    """PATH's key: its path in the readable code (zge/render/draw.cpp), with this host's separator, as os.path.relpath
    gave it from the one folder the readable code was in before layout1 (outside it: the path from TREE)."""
    key = zlayout.readable_key(tree, path)
    return key.replace('/', os.sep) if key is not None else os.path.relpath(path, tree)


def readable(tree, *parts):
    """The file the key PARTS name (os.path.join's parts: 'dm2', 'bound.tsv'), wherever its folder is; a name of the old
    src/game's own files (plain_sources.txt) is games/'s."""
    key = '/'.join(p.replace(os.sep, '/').strip('/') for p in parts if p)
    path = zlayout.readable_path(tree, key)
    return path if path is not None else os.path.join(tree, 'games', *key.split('/'))


def is_readable(path, tree=TREE):
    return zlayout.readable_key(tree, path) is not None


def is_tree(path):
    """Whether PATH is a z-axis checkout (its readable code in engine/, ps2/, games/), not one folder of sources."""
    return bool(path) and os.path.isdir(os.path.join(path, 'engine', 'zge'))


def walk(root, *key):
    """os.walk of the readable code when ROOT is a tree (zlayout.readable_walk: its seven folders, a game's own beside
    its overrides/, tools/, docs/ and workspace/; with KEY, the folder it names: 'aggressive', 'zge', 'render'), else
    of the folder ROOT (drafts, a test's sources; with KEY, os.path.join(ROOT, *KEY)); dirs sorted."""
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
