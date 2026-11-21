"""The layout of a z-axis tree, named in one place for the tools that read a tree (a lane's branch, a commit a worker
builds). No side effects on import; paths.sh's zdir and zreadable are the shell's.

    import zlayout
    zlayout.path(tree, 'ps2', 'runtime')          # TREE/ps2/runtime
    zlayout.readable_path(tree, 'zge/render/draw.cpp')   # TREE/engine/zge/render/draw.cpp
    zlayout.readable_key(tree, path)              # 'zge/render/draw.cpp', or None outside the readable code
    zlayout.readable_files(tree, ('.cpp', '.h'))  # [(key, path)] in key order

The readable code is in seven folders of three top-level ones: the engine's (engine/zge, and engine/host, the host's
services to it), the PS2's (ps2/ee, the EE as readable code sees it, and ps2/sce, Sony's libraries) and each game's
(games/aggressive, games/dm2, games/bmx: the game's code at the top, beside its overrides/, tools/, docs/ and
workspace/, which are not readable code). A readable source is known by its key: its path from its top-level folder
(zge/render/draw.cpp, ee/readable.h, aggressive/skater/air.cpp), as before the layout of layout1 put the seven
folders apart (they were one, runtime/src/game). The keys are what the symbol headers, the tables, the baselines and
the work database name; sorted by key, the sources are in the order the build compiles and registers them.

A tree from before layout1 (its runtime, tooling and android folders) is not read.
"""
import os

# kind: the top-level folder's names (one: the tuple stays for the callers that go through every name)
FOLDERS = {k: (k,) for k in ('engine', 'ps2', 'games', 'collection', 'platform', 'tools', 'recompiler', 'iop',
                             'third_party', 'tests', 'docs')}
# The readable code: a key's first folder and where it is
READABLE = (('aggressive', 'games/aggressive'), ('bmx', 'games/bmx'), ('dm2', 'games/dm2'),
            ('ee', 'ps2/ee'), ('host', 'engine/host'), ('sce', 'ps2/sce'), ('zge', 'engine/zge'))
READABLE_TOPS = dict(READABLE)
GAMES = ('aggressive', 'dm2', 'bmx')
# A game's folders beside its code: not readable code
GAME_NOT_CODE = ('overrides', 'tools', 'docs', 'workspace')
# The include roots of the readable code: what an #include "zge/..." or "aggressive/..." is found under
READABLE_ROOTS = ('engine', 'ps2', 'games')


def folder(tree, kind):
    return FOLDERS[kind][0]


def path(tree, kind, *rest):
    return os.path.join(tree, FOLDERS[kind][0], *rest)


def readable_dirs(tree):
    """[(top, folder)]: the seven readable folders in key order."""
    return [(top, os.path.join(tree, rel)) for top, rel in READABLE]


def readable_path(tree, key):
    """The file a key names (it need not exist), or None when its first folder is none of the seven."""
    top, _, rest = key.replace('\\', '/').partition('/')
    rel = READABLE_TOPS.get(top)
    if rel is None:
        return None
    return os.path.join(tree, rel, rest) if rest else os.path.join(tree, rel)


def readable_key(tree, path):
    """A path's key, or None when it is not readable code (outside the seven folders, or in a game's overrides/,
    tools/, docs/ or workspace/)."""
    rel = os.path.relpath(os.path.abspath(path), os.path.abspath(tree)).replace('\\', '/')
    for top, where in READABLE:
        if rel == where or rel.startswith(where + '/'):
            rest = rel[len(where) + 1:]
            if top in GAMES and rest.split('/', 1)[0] in GAME_NOT_CODE:
                return None
            return top + ('/' + rest if rest else '')
    return None


def readable_walk(tree, top=None):
    """os.walk over the readable folders (TOP's alone when given), skipping a game's overrides/, tools/, docs/ and
    workspace/: (folder, dirs, files) with dirs and files sorted."""
    for t, where in readable_dirs(tree):
        if top is not None and t != top:
            continue
        for d, dirs, files in os.walk(where):
            if t in GAMES and d == where:
                dirs[:] = [x for x in dirs if x not in GAME_NOT_CODE]
            dirs.sort()
            yield d, dirs, sorted(files)


def readable_files(tree, exts=None, top=None):
    """[(key, path)] of every readable file (with one of EXTS when given), sorted by key."""
    out = []
    for d, _, files in readable_walk(tree, top):
        for f in files:
            if exts is None or f.endswith(tuple(exts)):
                p = os.path.join(d, f)
                out.append((readable_key(tree, p), p))
    return sorted(out)


def overrides_dirs(tree):
    """[(owner, folder)]: the overrides, the engine's (zge_*) and each game's, in the order the runner links them
    (by their files' old names: aggressive_inline_, bmx_xxx, dave_mirra, zge_)."""
    return [('aggressive', path(tree, 'games', 'aggressive', 'overrides')),
            ('bmx', path(tree, 'games', 'bmx', 'overrides')),
            ('dm2', path(tree, 'games', 'dm2', 'overrides')),
            ('zge', path(tree, 'engine', 'overrides'))]


def overrides_files(tree, exts=('.cpp', '.h')):
    """[path] of every override source, in the runner's order."""
    out = []
    for _, d in overrides_dirs(tree):
        if os.path.isdir(d):
            out += [os.path.join(d, f) for f in sorted(os.listdir(d)) if f.endswith(tuple(exts))]
    return out


def folder_at(repo, sha, kind):
    """folder() for a commit, not a checkout."""
    return FOLDERS[kind][0]
