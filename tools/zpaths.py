"""The z-axis layout for Python tools: paths.env read as paths.sh reads it.

    import zpaths                  # with this folder (tools/) on sys.path
    zpaths.AI_WORKSPACE, zpaths.get('MERGE_OUT')
    zpaths.tree_path(tree, 'ps2', 'runtime')   # a z-axis tree's folder (checks/zlayout.py)
    zpaths.use('modding', 'dm2')   # the tools of tools/modding and games/dm2/tools importable by their names

A name set in the environment wins; ZAXIS_MAIN comes from git (the main checkout of the z-axis
checkout this tools/ folder sits in) before paths.env's fallback. ZAXIS_TOOLING is this folder (tools/), ZAXIS
the checkout it is in, ZAXIS_WORKSHOP the workshop's folder (workshop/ beside it, else the main
checkout's; None in a public checkout).
"""
import os
import re
import subprocess

ZAXIS_TOOLING = os.environ.get('ZAXIS_TOOLING') or os.path.dirname(os.path.abspath(__file__))
ZAXIS = os.environ.get('ZAXIS') or os.path.dirname(ZAXIS_TOOLING)


def _main_checkout():
    try:
        common = subprocess.run(['git', '-C', ZAXIS, 'rev-parse', '--path-format=absolute', '--git-common-dir'],
                                capture_output=True, text=True, timeout=10).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return None
    return common[:-len('/.git')] if common.endswith('/.git') else None


def _workshop():
    """The workshop's folder (merge, remote, status): $ZAXIS_WORKSHOP, this checkout's workshop/, else the main
    checkout's; None when there is none (a public checkout)."""
    if os.environ.get('ZAXIS_WORKSHOP'):
        return os.environ['ZAXIS_WORKSHOP']
    for root in (ZAXIS, os.environ.get('ZAXIS_MAIN') or _main_checkout()):
        if root and os.path.isdir(os.path.join(root, 'workshop', 'merge')):
            return os.path.join(root, 'workshop')
    return None


ZAXIS_WORKSHOP = _workshop()


def _user_home():
    """paths.env's $HOME: the user's own home (the password database), or ZAXIS_HOME, never the environment's HOME, so
    a tool run with a scratch HOME (a gate's runner, a test) still finds the data (tools28: BMX XXX did not boot)."""
    if os.environ.get('ZAXIS_HOME'):
        return os.environ['ZAXIS_HOME']
    try:
        import pwd
        return pwd.getpwuid(os.getuid()).pw_dir
    except (ImportError, KeyError):
        return os.path.expanduser('~')


USER_HOME = _user_home()

# Test runs make no sound (mute1, as paths.sh): every runner this process starts opens SDL's dummy audio device. A run
# that has to be heard says ZX_AUDIO=on itself.
os.environ.setdefault('ZX_AUDIO', 'off')


def load(path=os.path.join(ZAXIS_TOOLING, 'paths.env'),
         workshop=os.path.join(ZAXIS_WORKSHOP or ZAXIS_TOOLING, 'merge', 'paths.env')):
    """{NAME: path} for every name in paths.env, the environment first, then the workshop's layout (workshop/merge/paths.env,
    when there), then paths.env's defaults: the first value of a name wins, as paths.sh."""
    names = {'HOME': USER_HOME, 'ZAXIS_TOOLING': ZAXIS_TOOLING, 'ZAXIS': ZAXIS, 'ZAXIS_WORKSHOP': ZAXIS_WORKSHOP or ''}
    found = {'ZAXIS_MAIN': os.environ.get('ZAXIS_MAIN') or _main_checkout()}
    out = {}
    lines = open(workshop).read().splitlines() if os.path.isfile(workshop) else []
    for line in lines + open(path).read().splitlines():
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        name, value = line.split('=', 1)
        if name in out:
            continue
        value = os.environ.get(name) or found.get(name) or \
            re.sub(r'\$\{?(\w+)\}?', lambda m: names.get(m[1], os.environ.get(m[1], m[0])), value)
        names[name] = out[name] = value
    return out


def bind_dir(tree):
    """The bind tables a build of TREE uses: its own tools/readable/bind (committed since tools39), else the shared
    GAME_BIND_LEGACY of a tree from before; None for no tree."""
    if not tree:
        return None
    own = os.path.join(tree, 'tools', 'readable', 'bind')
    return own if os.path.isfile(os.path.join(own, 'dm2.tsv')) else PATHS.get('GAME_BIND_LEGACY')


def _tree():
    """The z-axis tree in use, as checks/ztree.py finds it: $ZAXIS_TREE, the checkout the working folder is in, the
    main checkout."""
    if os.environ.get('ZAXIS_TREE'):
        return os.environ['ZAXIS_TREE']
    here = os.getcwd()
    while True:
        if os.path.exists(os.path.join(here, '.git')) and os.path.isdir(os.path.join(here, 'engine', 'zge')):
            return here
        up = os.path.dirname(here)
        if up == here:
            return PATHS.get('ZAXIS_MAIN')
        here = up


PATHS = load()
# GAME_BIND_DIR: the environment's, else the tree in use's tables (bind_dir), never a shared folder for a tree that has
# its own: a commit builds and is read with its own tables.
PATHS['GAME_BIND_DIR'] = os.environ.get('GAME_BIND_DIR') or bind_dir(_tree()) or PATHS.get('GAME_BIND_LEGACY')
globals().update(PATHS)
# The workshop's own names (workshop/merge/paths.env), in the environment of every process this one starts, unless already set:
# a tool of another tooling (an older one, which reads paths.env alone) then finds the same folders (paths.sh exports
# every name already).
_WORKSHOP = os.path.join(ZAXIS_WORKSHOP or ZAXIS_TOOLING, 'merge', 'paths.env')
if os.path.isfile(_WORKSHOP):
    for _line in open(_WORKSHOP).read().splitlines():
        _name = _line.strip().split('=', 1)[0]
        if _line.strip() and not _line.lstrip().startswith('#') and _name in PATHS:
            os.environ.setdefault(_name, PATHS[_name])
# The tooling in use, for every process this one starts: a z-axis tool (tools/readable/zaxis_paths.py, which loads the
# main checkout's tools/zpaths.py) and a script of this tooling then read this paths.env, so a lane's tooling copy
# runs its own workspaces' scripts (tools34), as paths.sh's export does for the shell.
os.environ.setdefault('ZAXIS_TOOLING', ZAXIS_TOOLING)
if ZAXIS_WORKSHOP:
    os.environ.setdefault('ZAXIS_WORKSHOP', ZAXIS_WORKSHOP)


def _link_data(tree=ZAXIS):
    """The data links (setup/data-links.txt) of the tree in use, made when its first is missing and the data is here,
    as paths.sh does: a lane's copy (a git clone has none) reads and writes the same data as main's. Only missing links
    to data that exists, in a game folder the copy has (setup.sh does the rest)."""
    marker = os.path.join(PATHS.get('AI_WORKSPACE', ''), 'recomp', 'generated')
    ai = PATHS.get('AI_DATA')
    if os.path.islink(marker) or not ai or not os.path.exists(os.path.join(ai, 'recomp', 'generated')):
        return
    try:
        lines = open(os.path.join(ZAXIS_TOOLING, 'setup', 'data-links.txt')).read().splitlines()
    except OSError:
        return
    for line in lines:
        parts = line.split()
        if len(parts) != 3 or line.startswith('#') or not PATHS.get(parts[1]):
            continue
        link, target = os.path.join(tree, parts[0]), os.path.join(PATHS[parts[1]], parts[2])
        game = os.path.join(tree, *parts[0].split('/')[:2])
        if not os.path.isdir(game) or os.path.lexists(link) or not os.path.exists(target):
            continue
        try:
            os.makedirs(os.path.dirname(link), exist_ok=True)
            os.symlink(target, link)
        except OSError:     # (another process made it first, or a host without symlinks)
            pass


_link_data()


def generated(folder):
    """A game's generated folder as this tooling's builds use it: its real path with -GENERATED_SET after it
    (paths.env), or the folder itself when the set is empty (zgen in paths.sh)."""
    tag = os.environ.get('GENERATED_SET', PATHS.get('GENERATED_SET', ''))
    return os.path.realpath(folder) + '-' + tag if tag else folder


def get(name):
    return PATHS[name]


def _zlayout():
    import importlib.util
    spec = importlib.util.spec_from_file_location('zlayout', os.path.join(ZAXIS_TOOLING, 'checks', 'zlayout.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# A z-axis tree's folders (checks/zlayout.py): zpaths.tree_path(tree, 'ps2', 'runtime'); the readable code by its keys
# (zlayout.readable_path(tree, 'zge/render/draw.cpp'), readable_files, readable_key).
zlayout = _zlayout()
folder = zlayout.folder
tree_path = zlayout.path

# The tool folders, by name, of the tree in use (ZAXIS): use() puts them on sys.path, so a tool imports another
# folder's by its name (import ai_level after use('modding')).
TOOL_DIRS = {
    'tools': ZAXIS_TOOLING,
    'checks': os.path.join(ZAXIS_TOOLING, 'checks'),
    'readable': os.path.join(ZAXIS_TOOLING, 'readable'),
    'modding': os.path.join(ZAXIS_TOOLING, 'modding'),
    'setup': os.path.join(ZAXIS_TOOLING, 'setup'),
    'workdb': os.path.join(ZAXIS_TOOLING, 'workdb'),
    'playbot': os.path.join(ZAXIS_TOOLING, 'playbot'),
    'aggressive': os.path.join(ZAXIS, 'games', 'aggressive', 'tools'),
    'dm2': os.path.join(ZAXIS, 'games', 'dm2', 'tools'),
    'bmx': os.path.join(ZAXIS, 'games', 'bmx', 'tools'),
    'aggressive-playbot': os.path.join(ZAXIS, 'games', 'aggressive', 'tools', 'playbot'),
    'dm2-playbot': os.path.join(ZAXIS, 'games', 'dm2', 'tools', 'playbot'),
    'bmx-playbot': os.path.join(ZAXIS, 'games', 'bmx', 'tools', 'playbot'),
}


def use(*names):
    """The tool folders NAMES (TOOL_DIRS) on sys.path, after the importer's own folder: a module of another folder
    is then imported by its name. Returns the folders."""
    import sys
    dirs = [TOOL_DIRS[n] for n in names]
    for d in reversed(dirs):
        if d not in sys.path:
            sys.path.insert(1 if sys.path else 0, d)
    return dirs
